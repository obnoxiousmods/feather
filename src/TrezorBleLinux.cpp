// SPDX-License-Identifier: BSD-3-Clause
// SPDX-FileCopyrightText: The Monero Project

#include "TrezorBleLinux.h"

// Feather globs its sources, so this file is handed to the compiler on every
// platform and has to exclude itself.
//
// Q_MOC_RUN is part of the condition deliberately. moc preprocesses this file
// without the compiler's platform macros defined, so a plain __linux__ guard
// would hide the Q_OBJECT class below from it; moc would then generate nothing,
// with no diagnostic, and the notification slot would never be connected at run
// time. moc defines Q_MOC_RUN for itself, so it always sees the class while the
// compiler still skips the whole file everywhere but Linux.
#if defined(__linux__) || defined(Q_MOC_RUN)

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVariantMap>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

#include "device_trezor/trezor/transport_ble.hpp"

// A named namespace, not an anonymous one: moc cannot see a Q_OBJECT class
// inside an anonymous namespace and silently generates nothing for it, leaving
// the notification slot unconnected at run time.
namespace trezor_ble_linux {

// BlueZ D-Bus vocabulary.
const char *kBluezService = "org.bluez";
const char *kObjectManager = "org.freedesktop.DBus.ObjectManager";
const char *kProperties = "org.freedesktop.DBus.Properties";
const char *kAdapterIface = "org.bluez.Adapter1";
const char *kDeviceIface = "org.bluez.Device1";
const char *kGattServiceIface = "org.bluez.GattService1";
const char *kGattCharIface = "org.bluez.GattCharacteristic1";

// The Trezor GATT profile. These mirror the TREZOR_*_UUID strings in
// transport_ble.hpp; BlueZ reports UUIDs lower-cased, so they are compared that
// way throughout.
const char *kServiceUuid = "8c000001-a59b-4d58-a9ad-073df69fa1b1";
const char *kWriteUuid = "8c000002-a59b-4d58-a9ad-073df69fa1b1";  // host -> device
const char *kNotifyUuid = "8c000003-a59b-4d58-a9ad-073df69fa1b1"; // device -> host

using ManagedObjects = QMap<QDBusObjectPath, QMap<QString, QVariantMap>>;

/**
 * Ask BlueZ for its whole object tree.
 *
 * BlueZ publishes adapters, devices, services and characteristics as one tree,
 * so a single call gives everything needed to find a device and its
 * characteristics without walking the hierarchy by hand.
 */
ManagedObjects managedObjects(const QDBusConnection &bus) {
    ManagedObjects objects;

    QDBusMessage call = QDBusMessage::createMethodCall(kBluezService, "/", kObjectManager,
                                                       "GetManagedObjects");
    QDBusMessage reply = bus.call(call, QDBus::Block, 5000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return objects;
    }

    const QDBusArgument arg = reply.arguments().first().value<QDBusArgument>();
    arg >> objects;
    return objects;
}

/** Path of the first powered Bluetooth adapter, or empty if there is none. */
QString findAdapter(const QDBusConnection &bus, const ManagedObjects &objects) {
    Q_UNUSED(bus)
    for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
        const auto &interfaces = it.value();
        if (!interfaces.contains(kAdapterIface)) continue;
        return it.key().path();
    }
    return {};
}

/** True when `uuids` contains the Trezor service, however it is cased. */
bool advertisesTrezorService(const QVariant &uuidsValue) {
    const QStringList uuids = uuidsValue.toStringList();
    for (const QString &uuid : uuids) {
        if (uuid.compare(QLatin1String(kServiceUuid), Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}

/**
 * Receives characteristic notifications and queues them for blocking reads.
 *
 * BlueZ delivers notification payloads as PropertiesChanged signals carrying the
 * characteristic's new Value, so this listens for those rather than for a
 * dedicated notification signal.
 */
class NotificationQueue : public QObject {
    Q_OBJECT

public:
    explicit NotificationQueue(QObject *parent = nullptr) : QObject(parent) {}

    /** Returns an empty vector on timeout or once cancelled. */
    std::vector<uint8_t> pop(unsigned timeoutMs) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                           [this] { return !m_queue.empty() || m_cancelled; })) {
            return {};
        }
        if (m_queue.empty()) return {};
        std::vector<uint8_t> out = std::move(m_queue.front());
        m_queue.pop_front();
        return out;
    }

    void cancel() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cancelled = true;
        }
        m_cv.notify_all();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.clear();
        m_cancelled = false;
    }

public slots:
    void onPropertiesChanged(const QString &interfaceName, const QVariantMap &changed,
                             const QStringList &) {
        if (interfaceName != QLatin1String(kGattCharIface)) return;
        if (!changed.contains(QStringLiteral("Value"))) return;

        const QByteArray value = changed.value(QStringLiteral("Value")).toByteArray();
        if (value.isEmpty()) return;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.emplace_back(value.begin(), value.end());
        }
        m_cv.notify_one();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::vector<uint8_t>> m_queue;
    bool m_cancelled = false;
};

/**
 * BlueZ implementation of the transport's Bluetooth backend.
 *
 * The shape follows the same sequence the Trezor's own client uses and that the
 * Windows backend arrived at the hard way: scan for advertisements carrying the
 * service UUID, connect, wait for services to resolve, subscribe to
 * notifications, and write *with* response in fixed 244-byte packets.
 */
class BluezBleBackend : public hw::trezor::ble::BleBackend {
public:
    BluezBleBackend() : m_bus(QDBusConnection::systemBus()) {}

    ~BluezBleBackend() override {
        try {
            disconnect();
        } catch (...) {
            // Nothing useful to do while unwinding.
        }
    }

    std::vector<hw::trezor::ble::BleDeviceInfo> enumerate() override {
        std::vector<hw::trezor::ble::BleDeviceInfo> found;
        if (!m_bus.isConnected()) return found;

        ManagedObjects objects = managedObjects(m_bus);
        const QString adapter = findAdapter(m_bus, objects);
        if (adapter.isEmpty()) return found;

        startDiscovery(adapter);

        // A device only advertises while in pairing mode and can take tens of
        // seconds to be heard, so when the user has explicitly asked for
        // Bluetooth the scan is given a long window and returns the moment
        // something answers.
        const bool active = hw::trezor::ble::active_search();
        const unsigned budgetMs = active ? 45000 : 3000;

        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < budgetMs) {
            objects = managedObjects(m_bus);
            found = collectTrezors(objects);
            if (!found.empty()) break;
            QThread::msleep(400);
        }

        stopDiscovery(adapter);
        return found;
    }

    void connect(const std::string &address) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        disconnectLocked();

        CHECK_AND_ASSERT_THROW_MES(m_bus.isConnected(),
                                   "BLE: cannot reach the system bus; is BlueZ running?");

        const QString devicePath = devicePathForAddress(QString::fromStdString(address));
        CHECK_AND_ASSERT_THROW_MES(!devicePath.isEmpty(),
                                   "BLE: the device is no longer visible; put it back into "
                                   "Bluetooth pairing mode");

        connectDevice(devicePath);
        resolveCharacteristics(devicePath);
        subscribe();

        m_devicePath = devicePath;
        m_connected = true;
    }

    void disconnect() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        disconnectLocked();
    }

    bool is_connected() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_connected;
    }

    void write_packet(const uint8_t *data, size_t len) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        CHECK_AND_ASSERT_THROW_MES(m_connected, "BLE: device is not connected");

        QDBusInterface characteristic(kBluezService, m_writePath, kGattCharIface, m_bus);
        QVariantMap options;
        // Write with response. The device's write characteristic expects an
        // acknowledged write; the Trezor's own client does the same.
        options.insert(QStringLiteral("type"), QStringLiteral("request"));

        const QByteArray payload(reinterpret_cast<const char *>(data), static_cast<int>(len));
        QDBusReply<void> reply = characteristic.call(QStringLiteral("WriteValue"), payload,
                                                     options);
        CHECK_AND_ASSERT_THROW_MES(reply.isValid(), "BLE: writing to the device failed: "
                                                        << reply.error().message().toStdString());
    }

    size_t read_packet(uint8_t *out, size_t maxLen, unsigned timeoutMs) override {
        // Deliberately not holding m_mutex: this blocks, and the notification
        // slot that feeds the queue must stay free to run.
        NotificationQueue *queue = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            CHECK_AND_ASSERT_THROW_MES(m_connected && m_notifications,
                                       "BLE: device is not connected");
            queue = m_notifications;
        }

        const std::vector<uint8_t> packet = queue->pop(timeoutMs);
        if (packet.empty()) return 0;

        const size_t n = std::min(packet.size(), maxLen);
        memcpy(out, packet.data(), n);
        if (n < maxLen) {
            // The framing layer reads an explicit length and a checksum, so
            // zero-filling a trimmed tail is harmless and keeps the caller's
            // fixed-size contract.
            memset(out + n, 0, maxLen - n);
        }
        return maxLen;
    }

    size_t negotiated_packet_size() const override {
        // Fixed at the protocol's 244 rather than derived from the negotiated
        // MTU: the firmware packs to 244 regardless, and both ends have to agree
        // on one number.
        return hw::trezor::ble::BLE_PACKET_SIZE;
    }

private:
    std::vector<hw::trezor::ble::BleDeviceInfo> collectTrezors(const ManagedObjects &objects) {
        std::vector<hw::trezor::ble::BleDeviceInfo> found;
        for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
            const auto &interfaces = it.value();
            if (!interfaces.contains(kDeviceIface)) continue;

            const QVariantMap props = interfaces.value(kDeviceIface);
            if (!advertisesTrezorService(props.value(QStringLiteral("UUIDs")))) continue;

            hw::trezor::ble::BleDeviceInfo info;
            info.address = props.value(QStringLiteral("Address")).toString().toStdString();
            const QString name = props.value(QStringLiteral("Name")).toString();
            info.name = name.isEmpty() ? "Trezor" : name.toStdString();
            info.paired = props.value(QStringLiteral("Paired")).toBool();
            if (!info.address.empty()) found.push_back(std::move(info));
        }
        return found;
    }

    void startDiscovery(const QString &adapterPath) {
        QDBusInterface adapter(kBluezService, adapterPath, kAdapterIface, m_bus);

        // Filtering on the service UUID keeps the scan cheap and stops unrelated
        // peripherals from crowding the results. Transport "le" avoids classic
        // Bluetooth inquiry entirely.
        QVariantMap filter;
        filter.insert(QStringLiteral("UUIDs"), QStringList{QLatin1String(kServiceUuid)});
        filter.insert(QStringLiteral("Transport"), QStringLiteral("le"));
        adapter.call(QStringLiteral("SetDiscoveryFilter"), filter);
        adapter.call(QStringLiteral("StartDiscovery"));
    }

    void stopDiscovery(const QString &adapterPath) {
        QDBusInterface adapter(kBluezService, adapterPath, kAdapterIface, m_bus);
        adapter.call(QStringLiteral("StopDiscovery"));
    }

    QString devicePathForAddress(const QString &address) {
        const ManagedObjects objects = managedObjects(m_bus);
        for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
            const auto &interfaces = it.value();
            if (!interfaces.contains(kDeviceIface)) continue;
            const QVariantMap props = interfaces.value(kDeviceIface);
            if (props.value(QStringLiteral("Address")).toString().compare(
                    address, Qt::CaseInsensitive) == 0) {
                return it.key().path();
            }
        }
        return {};
    }

    /**
     * Bring the link up and wait for the GATT tree to appear.
     *
     * BlueZ resolves services asynchronously after connecting, so the
     * characteristics simply are not in the object tree yet when Connect()
     * returns. ServicesResolved is what says they are.
     */
    void connectDevice(const QString &devicePath) {
        QDBusInterface device(kBluezService, devicePath, kDeviceIface, m_bus);

        // Not calling Pair() here. Pairing is requested only when the device
        // demands an authenticated link; asking up front is what the Trezor's
        // own client deliberately avoids outside Android, and BlueZ raises the
        // security level by itself when a characteristic requires it.
        QDBusReply<void> reply = device.call(QStringLiteral("Connect"));
        CHECK_AND_ASSERT_THROW_MES(reply.isValid(),
                                   "BLE: could not connect to the device: "
                                       << reply.error().message().toStdString());

        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 20000) {
            QDBusInterface props(kBluezService, devicePath, kProperties, m_bus);
            QDBusReply<QVariant> resolved =
                    props.call(QStringLiteral("Get"), QLatin1String(kDeviceIface),
                               QStringLiteral("ServicesResolved"));
            if (resolved.isValid() && resolved.value().toBool()) return;
            QThread::msleep(200);
        }
        CHECK_AND_ASSERT_THROW_MES(false, "BLE: the device connected but never published its "
                                          "services");
    }

    void resolveCharacteristics(const QString &devicePath) {
        const ManagedObjects objects = managedObjects(m_bus);

        // Find the Trezor service belonging to this device, then the two
        // characteristics underneath it. Paths are hierarchical, so a
        // characteristic belongs to the service its path starts with.
        QString servicePath;
        for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
            if (!it.value().contains(kGattServiceIface)) continue;
            if (!it.key().path().startsWith(devicePath)) continue;
            const QVariantMap props = it.value().value(kGattServiceIface);
            if (props.value(QStringLiteral("UUID")).toString().compare(
                    QLatin1String(kServiceUuid), Qt::CaseInsensitive) == 0) {
                servicePath = it.key().path();
                break;
            }
        }
        CHECK_AND_ASSERT_THROW_MES(!servicePath.isEmpty(),
                                   "BLE: the device does not expose the Trezor service");

        m_writePath.clear();
        m_notifyPath.clear();
        for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
            if (!it.value().contains(kGattCharIface)) continue;
            if (!it.key().path().startsWith(servicePath)) continue;

            const QString uuid = it.value().value(kGattCharIface)
                                         .value(QStringLiteral("UUID")).toString();
            if (uuid.compare(QLatin1String(kWriteUuid), Qt::CaseInsensitive) == 0) {
                m_writePath = it.key().path();
            } else if (uuid.compare(QLatin1String(kNotifyUuid), Qt::CaseInsensitive) == 0) {
                m_notifyPath = it.key().path();
            }
        }
        CHECK_AND_ASSERT_THROW_MES(!m_writePath.isEmpty() && !m_notifyPath.isEmpty(),
                                   "BLE: the device is missing its Trezor characteristics");
    }

    void subscribe() {
        m_notifications = new NotificationQueue();

        const bool connected = m_bus.connect(
                kBluezService, m_notifyPath, kProperties, QStringLiteral("PropertiesChanged"),
                m_notifications,
                SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
        if (!connected) {
            delete m_notifications;
            m_notifications = nullptr;
            CHECK_AND_ASSERT_THROW_MES(false,
                                       "BLE: could not listen for device notifications");
        }

        QDBusInterface characteristic(kBluezService, m_notifyPath, kGattCharIface, m_bus);
        QDBusReply<void> reply = characteristic.call(QStringLiteral("StartNotify"));
        if (!reply.isValid()) {
            teardownNotifications();
            CHECK_AND_ASSERT_THROW_MES(false, "BLE: the device refused our notification "
                                              "request: "
                                                  << reply.error().message().toStdString());
        }
    }

    void teardownNotifications() {
        if (!m_notifications) return;
        m_notifications->cancel();
        m_bus.disconnect(kBluezService, m_notifyPath, kProperties,
                         QStringLiteral("PropertiesChanged"), m_notifications,
                         SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
        delete m_notifications;
        m_notifications = nullptr;
    }

    /** Caller must hold m_mutex. */
    void disconnectLocked() {
        if (m_notifications && !m_notifyPath.isEmpty()) {
            QDBusInterface characteristic(kBluezService, m_notifyPath, kGattCharIface, m_bus);
            characteristic.call(QStringLiteral("StopNotify"));
        }
        teardownNotifications();

        if (!m_devicePath.isEmpty()) {
            QDBusInterface device(kBluezService, m_devicePath, kDeviceIface, m_bus);
            device.call(QStringLiteral("Disconnect"));
        }

        m_devicePath.clear();
        m_writePath.clear();
        m_notifyPath.clear();
        m_connected = false;
    }

    mutable std::mutex m_mutex;
    QDBusConnection m_bus;
    bool m_connected = false;
    QString m_devicePath;
    QString m_writePath;
    QString m_notifyPath;
    NotificationQueue *m_notifications = nullptr;
};

} // namespace trezor_ble_linux

using namespace trezor_ble_linux;

#include "TrezorBleLinux.moc"

void installTrezorBleLinuxBackend() {
    qDBusRegisterMetaType<QMap<QString, QVariantMap>>();
    qDBusRegisterMetaType<ManagedObjects>();

    hw::trezor::ble::set_backend_factory(
            []() -> std::shared_ptr<hw::trezor::ble::BleBackend> {
                return std::make_shared<BluezBleBackend>();
            });
}

#endif // __linux__ || Q_MOC_RUN
