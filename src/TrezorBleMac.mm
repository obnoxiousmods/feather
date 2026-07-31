// SPDX-License-Identifier: BSD-3-Clause
// SPDX-FileCopyrightText: The Monero Project

#include "TrezorBleMac.h"

#if defined(__APPLE__)

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "device_trezor/trezor/transport_ble.hpp"

// The Trezor GATT profile, mirroring the TREZOR_*_UUID strings in
// transport_ble.hpp.
static NSString *const kTrezorServiceUuid = @"8C000001-A59B-4D58-A9AD-073DF69FA1B1";
static NSString *const kTrezorWriteUuid = @"8C000002-A59B-4D58-A9AD-073DF69FA1B1";
static NSString *const kTrezorNotifyUuid = @"8C000003-A59B-4D58-A9AD-073DF69FA1B1";

namespace {

/**
 * Thread-safe queue of notification payloads.
 *
 * CoreBluetooth delivers callbacks on a dispatch queue while the transport reads
 * from a wallet thread, so the two are decoupled here rather than by trying to
 * make the transport reentrant.
 */
class PacketQueue {
public:
    void push(const uint8_t *data, size_t len) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.emplace_back(data, data + len);
        }
        m_cv.notify_one();
    }

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

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::vector<uint8_t>> m_queue;
    bool m_cancelled = false;
};

/** One discovered peripheral, kept alive so it can be connected to later. */
struct DiscoveredPeripheral {
    std::string identifier;
    std::string name;
};

} // namespace

/**
 * CoreBluetooth delegate.
 *
 * CoreBluetooth is entirely callback-driven, whereas the transport wants
 * blocking calls. Each asynchronous step therefore signals a condition variable
 * that the corresponding blocking method waits on.
 */
@interface TrezorBleDelegate : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property(nonatomic, strong) CBCentralManager *central;
@property(nonatomic, strong) CBPeripheral *peripheral;
@property(nonatomic, strong) CBCharacteristic *writeCharacteristic;
@property(nonatomic, strong) CBCharacteristic *notifyCharacteristic;
@property(nonatomic, strong) NSMutableDictionary<NSString *, CBPeripheral *> *discovered;
@end

@implementation TrezorBleDelegate {
@public
    std::mutex _mutex;
    std::condition_variable _cv;
    bool _poweredOn;
    bool _connected;
    bool _servicesResolved;
    bool _subscribed;
    bool _failed;
    std::string _failureReason;
    PacketQueue *_queue;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _poweredOn = false;
        _connected = false;
        _servicesResolved = false;
        _subscribed = false;
        _failed = false;
        _queue = nullptr;
        self.discovered = [NSMutableDictionary dictionary];
        self.central = [[CBCentralManager alloc] initWithDelegate:self queue:nil];
    }
    return self;
}

- (void)signal {
    _cv.notify_all();
}

// --- CBCentralManagerDelegate ---------------------------------------------

- (void)centralManagerDidUpdateState:(CBCentralManager *)central {
    std::lock_guard<std::mutex> lock(_mutex);
    _poweredOn = (central.state == CBManagerStatePoweredOn);
    [self signal];
}

- (void)centralManager:(CBCentralManager *)central
 didDiscoverPeripheral:(CBPeripheral *)peripheral
     advertisementData:(NSDictionary<NSString *, id> *)advertisementData
                  RSSI:(NSNumber *)RSSI {
    NSString *key = peripheral.identifier.UUIDString;
    @synchronized(self.discovered) {
        self.discovered[key] = peripheral;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    [self signal];
}

- (void)centralManager:(CBCentralManager *)central
  didConnectPeripheral:(CBPeripheral *)peripheral {
    peripheral.delegate = self;
    // Passing the service UUID restricts discovery to what we actually need.
    [peripheral discoverServices:@[ [CBUUID UUIDWithString:kTrezorServiceUuid] ]];
    std::lock_guard<std::mutex> lock(_mutex);
    _connected = true;
    [self signal];
}

- (void)centralManager:(CBCentralManager *)central
didFailToConnectPeripheral:(CBPeripheral *)peripheral
                 error:(NSError *)error {
    std::lock_guard<std::mutex> lock(_mutex);
    _failed = true;
    _failureReason = error ? error.localizedDescription.UTF8String : "connection failed";
    [self signal];
}

- (void)centralManager:(CBCentralManager *)central
didDisconnectPeripheral:(CBPeripheral *)peripheral
                 error:(NSError *)error {
    std::lock_guard<std::mutex> lock(_mutex);
    _connected = false;
    _servicesResolved = false;
    _subscribed = false;
    if (_queue) _queue->cancel();
    [self signal];
}

// --- CBPeripheralDelegate --------------------------------------------------

- (void)peripheral:(CBPeripheral *)peripheral
didDiscoverServices:(NSError *)error {
    if (error) {
        std::lock_guard<std::mutex> lock(_mutex);
        _failed = true;
        _failureReason = error.localizedDescription.UTF8String;
        [self signal];
        return;
    }
    for (CBService *service in peripheral.services) {
        if ([service.UUID isEqual:[CBUUID UUIDWithString:kTrezorServiceUuid]]) {
            [peripheral discoverCharacteristics:@[
                [CBUUID UUIDWithString:kTrezorWriteUuid],
                [CBUUID UUIDWithString:kTrezorNotifyUuid]
            ]
                                     forService:service];
            return;
        }
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _failed = true;
    _failureReason = "the device does not expose the Trezor service";
    [self signal];
}

- (void)peripheral:(CBPeripheral *)peripheral
didDiscoverCharacteristicsForService:(CBService *)service
             error:(NSError *)error {
    if (error) {
        std::lock_guard<std::mutex> lock(_mutex);
        _failed = true;
        _failureReason = error.localizedDescription.UTF8String;
        [self signal];
        return;
    }
    for (CBCharacteristic *characteristic in service.characteristics) {
        if ([characteristic.UUID isEqual:[CBUUID UUIDWithString:kTrezorWriteUuid]]) {
            self.writeCharacteristic = characteristic;
        } else if ([characteristic.UUID isEqual:[CBUUID UUIDWithString:kTrezorNotifyUuid]]) {
            self.notifyCharacteristic = characteristic;
        }
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (self.writeCharacteristic && self.notifyCharacteristic) {
        _servicesResolved = true;
    } else {
        _failed = true;
        _failureReason = "the device is missing its Trezor characteristics";
    }
    [self signal];
}

- (void)peripheral:(CBPeripheral *)peripheral
didUpdateNotificationStateForCharacteristic:(CBCharacteristic *)characteristic
             error:(NSError *)error {
    std::lock_guard<std::mutex> lock(_mutex);
    if (error) {
        _failed = true;
        _failureReason = error.localizedDescription.UTF8String;
    } else {
        _subscribed = characteristic.isNotifying;
    }
    [self signal];
}

- (void)peripheral:(CBPeripheral *)peripheral
didUpdateValueForCharacteristic:(CBCharacteristic *)characteristic
             error:(NSError *)error {
    if (error || !characteristic.value) return;
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_queue) return;
    const NSData *data = characteristic.value;
    _queue->push(static_cast<const uint8_t *>(data.bytes), data.length);
}

@end

namespace {

/**
 * CoreBluetooth implementation of the transport's Bluetooth backend.
 *
 * Follows the same sequence as the other platforms: scan for advertisements
 * carrying the service UUID, connect, discover characteristics, subscribe, and
 * write with response in fixed 244-byte packets. macOS arranges bonding itself
 * when the device asks for it, so there is no explicit pairing step.
 */
class CoreBluetoothBackend : public hw::trezor::ble::BleBackend {
public:
    CoreBluetoothBackend() {
        m_delegate = [[TrezorBleDelegate alloc] init];
        m_delegate->_queue = &m_queue;
    }

    ~CoreBluetoothBackend() override {
        try {
            disconnect();
        } catch (...) {
            // Nothing useful to do while unwinding.
        }
    }

    std::vector<hw::trezor::ble::BleDeviceInfo> enumerate() override {
        std::vector<hw::trezor::ble::BleDeviceInfo> found;
        if (!waitForPoweredOn(5000)) return found;

        [m_delegate.central
                scanForPeripheralsWithServices:@[ [CBUUID UUIDWithString:kTrezorServiceUuid] ]
                                       options:nil];

        // A device only advertises while in pairing mode and can take tens of
        // seconds to be heard, so an explicit hunt gets a long window.
        const unsigned budgetMs = hw::trezor::ble::active_search() ? 45000 : 3000;
        const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);

        while (std::chrono::steady_clock::now() < deadline) {
            @synchronized(m_delegate.discovered) {
                if (m_delegate.discovered.count > 0) break;
            }
            std::unique_lock<std::mutex> lock(m_delegate->_mutex);
            m_delegate->_cv.wait_for(lock, std::chrono::milliseconds(300));
        }

        [m_delegate.central stopScan];

        @synchronized(m_delegate.discovered) {
            for (NSString *key in m_delegate.discovered) {
                CBPeripheral *peripheral = m_delegate.discovered[key];
                hw::trezor::ble::BleDeviceInfo info;
                info.address = key.UTF8String;
                info.name = peripheral.name ? peripheral.name.UTF8String : "Trezor";
                info.paired = false;
                found.push_back(std::move(info));
            }
        }
        return found;
    }

    void connect(const std::string &address) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        disconnectLocked();

        CHECK_AND_ASSERT_THROW_MES(waitForPoweredOn(5000),
                                   "BLE: Bluetooth is not available on this system");

        NSString *key = [NSString stringWithUTF8String:address.c_str()];
        CBPeripheral *peripheral = nil;
        @synchronized(m_delegate.discovered) {
            peripheral = m_delegate.discovered[key];
        }
        CHECK_AND_ASSERT_THROW_MES(peripheral != nil,
                                   "BLE: the device is no longer visible; put it back into "
                                   "Bluetooth pairing mode");

        m_delegate.peripheral = peripheral;
        m_queue.reset();
        clearFlags();

        [m_delegate.central connectPeripheral:peripheral options:nil];
        CHECK_AND_ASSERT_THROW_MES(waitFor([&] { return m_delegate->_connected; }, 20000),
                                   "BLE: could not connect to the device"
                                       << failureSuffix());

        CHECK_AND_ASSERT_THROW_MES(
                waitFor([&] { return m_delegate->_servicesResolved; }, 20000),
                "BLE: the device connected but never published its services"
                    << failureSuffix());

        // No explicit pairing: macOS elevates security by itself when the
        // device demands an authenticated link.
        [m_delegate.peripheral setNotifyValue:YES
                            forCharacteristic:m_delegate.notifyCharacteristic];
        CHECK_AND_ASSERT_THROW_MES(waitFor([&] { return m_delegate->_subscribed; }, 20000),
                                   "BLE: the device refused our notification request"
                                       << failureSuffix());

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
        CHECK_AND_ASSERT_THROW_MES(m_connected && m_delegate.writeCharacteristic,
                                   "BLE: device is not connected");

        NSData *payload = [NSData dataWithBytes:data length:len];
        // With response, matching the Trezor client and the other backends.
        [m_delegate.peripheral writeValue:payload
                        forCharacteristic:m_delegate.writeCharacteristic
                                     type:CBCharacteristicWriteWithResponse];
    }

    size_t read_packet(uint8_t *out, size_t maxLen, unsigned timeoutMs) override {
        // Deliberately not holding m_mutex: this blocks, and the CoreBluetooth
        // callback that feeds the queue must stay free to run.
        const std::vector<uint8_t> packet = m_queue.pop(timeoutMs);
        if (packet.empty()) return 0;

        const size_t n = std::min(packet.size(), maxLen);
        memcpy(out, packet.data(), n);
        if (n < maxLen) {
            // The framing layer reads an explicit length and a checksum, so
            // zero-filling a trimmed tail is harmless.
            memset(out + n, 0, maxLen - n);
        }
        return maxLen;
    }

    size_t negotiated_packet_size() const override {
        // Fixed at the protocol's 244, as on every other platform.
        return hw::trezor::ble::BLE_PACKET_SIZE;
    }

private:
    void clearFlags() {
        std::lock_guard<std::mutex> lock(m_delegate->_mutex);
        m_delegate->_connected = false;
        m_delegate->_servicesResolved = false;
        m_delegate->_subscribed = false;
        m_delegate->_failed = false;
        m_delegate->_failureReason.clear();
    }

    std::string failureSuffix() {
        std::lock_guard<std::mutex> lock(m_delegate->_mutex);
        if (m_delegate->_failureReason.empty()) return {};
        return ": " + m_delegate->_failureReason;
    }

    template <typename Predicate>
    bool waitFor(Predicate ready, unsigned timeoutMs) {
        std::unique_lock<std::mutex> lock(m_delegate->_mutex);
        return m_delegate->_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                        [&] { return ready() || m_delegate->_failed; }) &&
               !m_delegate->_failed;
    }

    bool waitForPoweredOn(unsigned timeoutMs) {
        std::unique_lock<std::mutex> lock(m_delegate->_mutex);
        return m_delegate->_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                        [&] { return m_delegate->_poweredOn; });
    }

    /** Caller must hold m_mutex. */
    void disconnectLocked() {
        m_queue.cancel();
        if (m_delegate.peripheral) {
            if (m_delegate.notifyCharacteristic) {
                [m_delegate.peripheral setNotifyValue:NO
                                    forCharacteristic:m_delegate.notifyCharacteristic];
            }
            [m_delegate.central cancelPeripheralConnection:m_delegate.peripheral];
        }
        m_delegate.writeCharacteristic = nil;
        m_delegate.notifyCharacteristic = nil;
        m_connected = false;
    }

    mutable std::mutex m_mutex;
    bool m_connected = false;
    PacketQueue m_queue;
    TrezorBleDelegate *m_delegate = nil;
};

} // namespace

void installTrezorBleMacBackend() {
    hw::trezor::ble::set_backend_factory(
            []() -> std::shared_ptr<hw::trezor::ble::BleBackend> {
                return std::make_shared<CoreBluetoothBackend>();
            });
}

#endif
