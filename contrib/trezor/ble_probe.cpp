// Standalone diagnostic: can we reach a Trezor Safe 7 over Bluetooth Low Energy
// using the WinRT API surface?
//
// Why this exists
// ---------------
// The first Windows BLE backend used the Win32 GATT API (bluetoothleapis.h). That
// API can only ever see devices Windows has *bonded* -- it enumerates device
// interfaces created by the OS pairing flow. A Trezor advertising its custom GATT
// service is never bonded that way, so the enumeration was always empty no matter
// what the user did in Windows Settings.
//
// trezorlib (BleakScanner.discover) and Cake Wallet (universal_ble) both take the
// other route: scan raw BLE *advertisements*, filter by the Trezor service UUID,
// and connect directly. That works without any OS-level pairing. This probe
// reproduces that route on Windows via WinRT, which is the only Windows API that
// exposes advertisement scanning and connect-without-bonding.
//
// The probe walks the whole path and reports each step, so a failure tells us
// exactly where it broke:
//   1. scan advertisements                 -> is the device transmitting at all?
//   2. connect by Bluetooth address        -> can we reach it unbonded?
//   3. discover the Trezor GATT service    -> does it expose the expected service?
//   4. find the RX/TX characteristics      -> does it expose the expected pipes?
//   5. subscribe to notifications          -> can the device talk back to us?
//   6. THP channel allocation round trip   -> does end-to-end framing work?
//
// Step 6 is the real proof: it sends the exact same bytes the wallet's THP
// implementation sends, and a valid response means the transport is usable.
//
// Build:
//   x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -municode \
//       -o ble-probe.exe ble_probe_winrt.cpp \
//       -lwindowsapp -lruntimeobject -lole32

#include <windows.h>

// mingw-w64 14's windows.foundation.h declares IReference<BYTE> and
// IReference<boolean> as distinct template specialisations, but both `BYTE` and
// `boolean` resolve to `unsigned char`, so the second one is a redefinition and
// the header cannot compile as shipped. Claiming the guard of the later block
// keeps the earlier, equivalent specialisation and skips the duplicate. We do
// not use either type.
#define ____FIReference_1_boolean_INTERFACE_DEFINED__

#include <roapi.h>
#include <winstring.h>
#include <robuffer.h>
#include <windows.foundation.h>
#include <windows.devices.bluetooth.h>
#include <windows.devices.bluetooth.advertisement.h>
#include <windows.devices.bluetooth.genericattributeprofile.h>
#include <windows.devices.enumeration.h>
#include <windows.devices.radios.h>
#include <windows.storage.streams.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace wf = ABI::Windows::Foundation;
namespace wfc = ABI::Windows::Foundation::Collections;
namespace wdb = ABI::Windows::Devices::Bluetooth;
namespace wda = ABI::Windows::Devices::Bluetooth::Advertisement;
namespace wdg = ABI::Windows::Devices::Bluetooth::GenericAttributeProfile;
namespace wss = ABI::Windows::Storage::Streams;
namespace wde = ABI::Windows::Devices::Enumeration;

// ---------------------------------------------------------------------------
// Trezor BLE GATT profile
// ---------------------------------------------------------------------------

// 8c000001-a59b-4d58-a9ad-073df69fa1b1  service
// 8c000002-...                          RX: host writes here
// 8c000003-...                          TX: device notifies here
static const GUID TREZOR_SERVICE =
    {0x8c000001, 0xa59b, 0x4d58, {0xa9, 0xad, 0x07, 0x3d, 0xf6, 0x9f, 0xa1, 0xb1}};
static const GUID TREZOR_CHAR_RX =
    {0x8c000002, 0xa59b, 0x4d58, {0xa9, 0xad, 0x07, 0x3d, 0xf6, 0x9f, 0xa1, 0xb1}};
static const GUID TREZOR_CHAR_TX =
    {0x8c000003, 0xa59b, 0x4d58, {0xa9, 0xad, 0x07, 0x3d, 0xf6, 0x9f, 0xa1, 0xb1}};

// ---------------------------------------------------------------------------
// IBluetoothLEDevice3
//
// mingw-w64 14 forward-declares this interface but never defines it, so the
// async GATT discovery methods are unreachable through the shipped headers. The
// predecessor interface only offers the deprecated synchronous `GattServices`
// property, which reads a cache that is empty for a device we have never bonded
// with -- precisely our case.
//
// The vtable layout and IID below are transcribed from the Windows SDK IDL
// (Include/10.0.26100.0/winrt/windows.devices.bluetooth.idl). Declaring it by
// hand is safe because COM interfaces are immutable once published.
// ---------------------------------------------------------------------------

static const GUID IID_IBluetoothLEDevice3_local =
    {0xaee9e493, 0x44ac, 0x40dc, {0xaf, 0x33, 0xb2, 0xc1, 0x3c, 0x01, 0xca, 0x46}};

struct IBluetoothLEDevice3_local : public IInspectable {
  // Order must match the IDL exactly; the entries we do not use still have to
  // occupy their slots.
  virtual HRESULT STDMETHODCALLTYPE get_DeviceAccessInformation(void **value) = 0;
  virtual HRESULT STDMETHODCALLTYPE RequestAccessAsync(void **operation) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetGattServicesAsync(
      wf::IAsyncOperation<wdg::GattDeviceServicesResult *> **operation) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetGattServicesWithCacheModeAsync(
      wdb::BluetoothCacheMode cacheMode,
      wf::IAsyncOperation<wdg::GattDeviceServicesResult *> **operation) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetGattServicesForUuidAsync(
      GUID serviceUuid,
      wf::IAsyncOperation<wdg::GattDeviceServicesResult *> **operation) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetGattServicesForUuidWithCacheModeAsync(
      GUID serviceUuid, wdb::BluetoothCacheMode cacheMode,
      wf::IAsyncOperation<wdg::GattDeviceServicesResult *> **operation) = 0;
};

// IBluetoothLEDeviceStatics2, from the SDK IDL. mingw-w64 only defines the v1
// statics, whose FromBluetoothAddressAsync assumes a *public* address. The
// Trezor advertises a resolvable private (random) address, so connecting
// through the v1 call targets an address kind the device does not have and
// discovery comes back Unreachable. This overload takes the address type.
static const GUID IID_IBluetoothLEDeviceStatics2_local =
    {0x5f12c06b, 0x3bac, 0x43e8, {0xad, 0x16, 0x56, 0x32, 0x71, 0xbd, 0x41, 0xc2}};

struct IBluetoothLEDeviceStatics2_local : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE GetDeviceSelectorFromPairingState(boolean, HSTRING *) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceSelectorFromConnectionStatus(int, HSTRING *) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceSelectorFromDeviceName(HSTRING, HSTRING *) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceSelectorFromBluetoothAddress(UINT64, HSTRING *) = 0;
  virtual HRESULT STDMETHODCALLTYPE
      GetDeviceSelectorFromBluetoothAddressWithBluetoothAddressType(UINT64, int, HSTRING *) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceSelectorFromAppearance(void *, HSTRING *) = 0;
  virtual HRESULT STDMETHODCALLTYPE FromBluetoothAddressWithBluetoothAddressTypeAsync(
      UINT64 address, int addressType,
      wf::IAsyncOperation<wdb::BluetoothLEDevice *> **operation) = 0;
};

// Pairing interfaces, from the SDK IDL. mingw-w64 defines none of them, and its
// IDeviceInformation is truncated (no Pairing property, which actually lives on
// IDeviceInformation2 anyway).
//
// These are needed because the Trezor refuses to enable notifications over an
// unauthenticated link: writing the CCCD returns 0x80650005, which is ATT error
// 0x05 "Insufficient Authentication". The link has to be bonded first. This is
// the same connect-then-pair order trezorlib uses.
static const GUID IID_IDeviceInformation2_local =
    {0xf156a638, 0x7997, 0x48d9, {0xa1, 0x0c, 0x26, 0x9d, 0x46, 0x53, 0x3f, 0x48}};
static const GUID IID_IDeviceInformationPairing_local =
    {0x2c4769f5, 0xf684, 0x40d5, {0x84, 0x69, 0xe8, 0xdb, 0xaa, 0xb7, 0x04, 0x85}};

struct IDevicePairingResult_local : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Status(int *status) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ProtectionLevelUsed(int *value) = 0;
};

// Standard IAsyncOperation<T> vtable shape. Declared by hand because mingw has
// no instantiation for DevicePairingResult; the pointer PairAsync hands back is
// already this interface, and awaiting only needs IAsyncInfo, which is standard.
struct IAsyncOperationDevicePairingResult_local : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE put_Completed(void *handler) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Completed(void **handler) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetResults(IDevicePairingResult_local **result) = 0;
};

struct IDeviceInformationPairing_local : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_IsPaired(boolean *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_CanPair(boolean *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE PairAsync(
      IAsyncOperationDevicePairingResult_local **result) = 0;
  virtual HRESULT STDMETHODCALLTYPE PairWithProtectionLevelAsync(
      int minProtectionLevel, IAsyncOperationDevicePairingResult_local **result) = 0;
};

struct IDeviceInformation2_local : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Kind(int *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Pairing(IDeviceInformationPairing_local **value) = 0;
};

// Custom pairing. The plain PairAsync above uses the default ceremony, which
// wants the operating system's own pairing UI and fails outright in a process
// that has none. Custom pairing lets us declare which ceremonies we support and
// answer the request ourselves, which is what bleak (and therefore trezorlib)
// does on Windows.
static const GUID IID_IDeviceInformationPairing2_local =
    {0xf68612fd, 0x0aee, 0x4328, {0x85, 0xcc, 0x1c, 0x74, 0x2b, 0xb1, 0x79, 0x0d}};
static const GUID IID_IDeviceInformationCustomPairing_local =
    {0x85138c02, 0x4ee6, 0x4914, {0x83, 0x70, 0x10, 0x7a, 0x39, 0x14, 0x4c, 0x0e}};
static const GUID IID_IDevicePairingRequestedEventArgs_local =
    {0xf717fc56, 0xde6b, 0x487f, {0x83, 0x76, 0x01, 0x80, 0xac, 0xa6, 0x99, 0x63}};

// DevicePairingKinds bit flags.
enum {
  kPairingKindConfirmOnly = 1,
  kPairingKindDisplayPin = 2,
  kPairingKindProvidePin = 4,
  kPairingKindConfirmPinMatch = 8,
};

struct IDevicePairingRequestedEventArgs_local : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_DeviceInformation(void **value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_PairingKind(int *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Pin(HSTRING *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE Accept() = 0;
  virtual HRESULT STDMETHODCALLTYPE AcceptWithPin(HSTRING pin) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeferral(void **result) = 0;
};

struct IDeviceInformationCustomPairing_local : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE PairAsync(
      int pairingKindsSupported, IAsyncOperationDevicePairingResult_local **result) = 0;
  virtual HRESULT STDMETHODCALLTYPE PairWithProtectionLevelAsync(
      int pairingKindsSupported, int minProtectionLevel,
      IAsyncOperationDevicePairingResult_local **result) = 0;
  virtual HRESULT STDMETHODCALLTYPE PairWithProtectionLevelAndSettingsAsync(
      int pairingKindsSupported, int minProtectionLevel, void *settings,
      IAsyncOperationDevicePairingResult_local **result) = 0;
  virtual HRESULT STDMETHODCALLTYPE add_PairingRequested(IUnknown *handler,
                                                        EventRegistrationToken *token) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_PairingRequested(EventRegistrationToken token) = 0;
};

struct IDeviceInformationPairing2_local : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_ProtectionLevel(int *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Custom(
      IDeviceInformationCustomPairing_local **value) = 0;
};

/**
 * Accepts the pairing ceremony on our behalf.
 *
 * The parameterised IID of
 * ITypedEventHandler<DeviceInformationCustomPairing*, DevicePairingRequestedEventArgs*>
 * is published in no header available to this build. WinRT derives such IIDs
 * deterministically -- a UUIDv5 over the generic type signature under a fixed
 * namespace -- so it is computed offline instead (see piid.py, which validates
 * the derivation against a PIID the headers do publish).
 *
 * Answering QueryInterface for any IID at all does not work: the runtime probes
 * for IMarshal, and handing back this object breaks delegate registration with
 * 0x80004021. Only the three interfaces this really implements are claimed.
 */
class PairingRequestedHandler : public IUnknown {
public:
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG n = InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return (ULONG)n;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    // fa65231f-4178-5de1-b2cc-03e22d7702b4, derived by piid.py.
    static const GUID kPairingRequestedHandlerIid =
        {0xfa65231f, 0x4178, 0x5de1, {0xb2, 0xcc, 0x03, 0xe2, 0x2d, 0x77, 0x02, 0xb4}};
    static const GUID kAgile =
        {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
    if (IsEqualGUID(riid, IID_IUnknown) ||
        IsEqualGUID(riid, kPairingRequestedHandlerIid) || IsEqualGUID(riid, kAgile)) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  // Vtable slot 4: Invoke(sender, args).
  virtual HRESULT STDMETHODCALLTYPE Invoke(void * /*sender*/,
                                           IDevicePairingRequestedEventArgs_local *args) {
    if (!args) return S_OK;
    int kind = 0;
    args->get_PairingKind(&kind);

    // HStr is declared further down, so the raw API is used here.
    HSTRING pin = nullptr;
    args->get_Pin(&pin);
    std::string pin_text;
    if (pin) {
      UINT32 plen = 0;
      const wchar_t *praw = WindowsGetStringRawBuffer(pin, &plen);
      for (UINT32 i = 0; i < plen; ++i) pin_text += (char)praw[i];
      WindowsDeleteString(pin);
    }

    printf("    pairing ceremony: %s%s\n",
           kind == kPairingKindConfirmOnly       ? "ConfirmOnly"
           : kind == kPairingKindDisplayPin      ? "DisplayPin"
           : kind == kPairingKindConfirmPinMatch ? "ConfirmPinMatch"
           : kind == kPairingKindProvidePin      ? "ProvidePin"
                                                 : "other",
           pin_text.empty() ? "" : ("  pin=" + pin_text).c_str());

    // ConfirmOnly and DisplayPin need nothing from us; ConfirmPinMatch means the
    // device is showing the same digits and we simply agree.
    const HRESULT hr = args->Accept();
    printf("    accepted (hr=0x%08lx)\n", (unsigned long)hr);
    return S_OK;
  }

private:
  LONG refs_ = 1;
};

static const char *pairing_status_name(int s) {
  switch (s) {
    case 0: return "Paired";
    case 1: return "NotReadyToPair";
    case 2: return "NotPaired";
    case 3: return "AlreadyPaired";
    case 4: return "ConnectionRejected";
    case 5: return "TooManyConnections";
    case 6: return "HardwareFailure";
    case 7: return "AuthenticationTimeout";
    case 8: return "AuthenticationNotAllowed";
    case 9: return "AuthenticationFailure";
    case 10: return "NoSupportedProfiles";
    case 11: return "ProtectionLevelCouldNotBeMet";
    case 12: return "AccessDenied";
    case 13: return "InvalidCeremonyData";
    case 14: return "PairingCanceled";
    case 15: return "OperationAlreadyInProgress";
    case 16: return "RequiredHandlerNotRegistered";
    case 17: return "RejectedByHandler";
    case 18: return "RemoteDeviceHasAssociation";
    case 19: return "Failed";
    default: return "?";
  }
}

// IBluetoothLEAdvertisementWatcher3 (Windows 11 24H2+), from the SDK IDL.
// Controls which physical layers the scanner listens on. A BLE 5 peripheral
// advertising on the Coded PHY is completely invisible to a scanner that only
// listens on the uncoded 1M PHY, which is the default.
static const GUID IID_IBluetoothLEAdvertisementWatcher3_local =
    {0x14d980be, 0x4002, 0x5dbe, {0x85, 0x19, 0xff, 0xca, 0x6c, 0xa3, 0x89, 0xf0}};

struct IBluetoothLEAdvertisementWatcher3_local : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_UseUncoded1MPhy(boolean *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_UseUncoded1MPhy(boolean value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_UseCodedPhy(boolean *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_UseCodedPhy(boolean value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ScanParameters(void **value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_ScanParameters(void *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_UseHardwareFilter(boolean *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_UseHardwareFilter(boolean value) = 0;
};

// IBluetoothAdapter3 (Windows 10 2004+), from the SDK IDL. Reports whether the
// radio understands BLE 5 advertising extensions at all -- if it does not, a
// peripheral using them cannot be seen by any amount of software.
static const GUID IID_IBluetoothAdapter3_local =
    {0x8f8624e0, 0xcba9, 0x5211, {0x9f, 0x89, 0x3a, 0xac, 0x62, 0xb4, 0xc6, 0xb8}};

struct IBluetoothAdapter3_local : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_IsExtendedAdvertisingSupported(boolean *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_MaxAdvertisementDataLength(UINT32 *value) = 0;
};

// ---------------------------------------------------------------------------
// Small COM helpers
// ---------------------------------------------------------------------------

// IAgileObject is a marker interface. Implementing it tells the Windows runtime
// our callbacks may be invoked directly on a threadpool thread instead of being
// marshalled back to the apartment that registered them. Without it, a handler
// registered from a single-threaded apartment (which is what Qt sets up on the
// GUI thread) would only ever fire while a message pump is running -- and the
// transport code that waits for BLE packets does not pump messages.
static const GUID IID_IAgileObject_local =
    {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

template <typename T>
class ComPtr {
public:
  ComPtr() = default;
  ComPtr(const ComPtr &o) : p_(o.p_) { if (p_) p_->AddRef(); }
  ComPtr(ComPtr &&o) noexcept : p_(o.p_) { o.p_ = nullptr; }
  ~ComPtr() { reset(); }
  ComPtr &operator=(ComPtr o) { std::swap(p_, o.p_); return *this; }

  T **put() { reset(); return &p_; }
  void **put_void() { reset(); return reinterpret_cast<void **>(&p_); }
  T *get() const { return p_; }
  T *operator->() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }
  void reset() { if (p_) { p_->Release(); p_ = nullptr; } }

  template <typename U>
  HRESULT as(const IID &iid, ComPtr<U> &out) const {
    if (!p_) return E_POINTER;
    return p_->QueryInterface(iid, out.put_void());
  }

private:
  T *p_ = nullptr;
};

// RAII wrapper for HSTRING.
class HStr {
public:
  explicit HStr(const wchar_t *s) { WindowsCreateString(s, (UINT32)wcslen(s), &h_); }
  HStr() = default;
  ~HStr() { if (h_) WindowsDeleteString(h_); }
  HStr(const HStr &) = delete;
  HStr &operator=(const HStr &) = delete;
  HSTRING get() const { return h_; }
  HSTRING *put() { if (h_) { WindowsDeleteString(h_); h_ = nullptr; } return &h_; }
  std::string to_utf8() const {
    if (!h_) return {};
    UINT32 len = 0;
    const wchar_t *raw = WindowsGetStringRawBuffer(h_, &len);
    if (!len) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, raw, (int)len, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, raw, (int)len, &out[0], n, nullptr, nullptr);
    return out;
  }

private:
  HSTRING h_ = nullptr;
};

// Fetch a WinRT activation factory / statics interface for a runtime class.
template <typename T>
static HRESULT get_factory(const wchar_t *class_name, const IID &iid, ComPtr<T> &out) {
  HStr name(class_name);
  return RoGetActivationFactory(name.get(), iid, out.put_void());
}

// Wait for a WinRT async operation by polling IAsyncInfo.
//
// Polling rather than installing a completion handler is deliberate: the status
// is updated regardless of which apartment we are on, so this works identically
// whether the caller is MTA or a Qt STA thread, and it needs no message pump.
template <typename TOp>
static HRESULT await_op(ComPtr<TOp> &op, unsigned timeout_ms = 30000) {
  if (!op) return E_POINTER;
  // mingw declares IAsyncInfo and AsyncStatus at global scope rather than under
  // ABI::Windows::Foundation.
  ComPtr<::IAsyncInfo> info;
  HRESULT hr = op.as(__uuidof(::IAsyncInfo), info);
  if (FAILED(hr)) return hr;

  const DWORD deadline = GetTickCount() + timeout_ms;
  for (;;) {
    ::AsyncStatus status = ::Started;
    hr = info->get_Status(&status);
    if (FAILED(hr)) return hr;
    if (status == ::Completed) return S_OK;
    if (status == ::Error) {
      HRESULT err = S_OK;
      info->get_ErrorCode(&err);
      return FAILED(err) ? err : E_FAIL;
    }
    if (status == ::Canceled) return E_ABORT;
    if ((int)(GetTickCount() - deadline) >= 0) {
      info->Cancel();
      return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    Sleep(10);
  }
}

// ---------------------------------------------------------------------------
// Event handler base
//
// One tiny reference-counted COM object shared by both handlers we need. It
// answers QueryInterface for IUnknown, IAgileObject and its own interface IID.
// ---------------------------------------------------------------------------

static std::string guid_to_string(const GUID &g);  // defined below
static std::string mac_to_string(uint64_t addr);   // defined below

template <typename TInterface>
class HandlerBase : public TInterface {
public:
  explicit HandlerBase(const IID &self_iid) : iid_(self_iid) {}
  virtual ~HandlerBase() = default;

  ULONG STDMETHODCALLTYPE AddRef() override {
    return (ULONG)InterlockedIncrement(&refs_);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG n = InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return (ULONG)n;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, iid_) ||
        IsEqualGUID(riid, IID_IAgileObject_local)) {
      *ppv = static_cast<TInterface *>(this);
      AddRef();
      return S_OK;
    }
    // Diagnostic: if the runtime wants an interface we refuse, it may silently
    // decline to deliver events. Show what it asked for.
    static std::atomic<int> refused{0};
    if (++refused <= 12) {
      printf("      [QI refused %s]\n", guid_to_string(riid).c_str());
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

private:
  LONG refs_ = 1;
  IID iid_;
};

// ---------------------------------------------------------------------------
// Advertisement scanning
// ---------------------------------------------------------------------------

struct SeenDevice {
  uint64_t address = 0;
  std::string name;
  int16_t rssi = 0;
  bool has_trezor_service = false;
  int reports = 0;
  std::vector<GUID> service_uuids;
  std::vector<uint16_t> manufacturer_ids;
  // Every distinct raw AD structure seen, as "0xTT hexbytes". Dumping these
  // shows exactly what a device transmits, including anything the parsed
  // properties above do not surface.
  std::vector<std::string> raw_sections;
  // BluetoothLEAdvertisementType: 0=ConnectableUndirected 1=ConnectableDirected
  // 2=ScannableUndirected 3=NonConnectableUndirected 4=ScanResponse
  // 5=Extended
  int last_adv_type = -1;
  // BluetoothAddressType: 0=Public 1=Random 2=Unspecified. Must be carried to
  // the connect call; the Trezor uses a rotating random address.
  int addr_type = 2;
};

// Raw count of Received callbacks, incremented before any filtering.
static std::atomic<long> g_raw_events{0};

static std::string guid_to_string(const GUID &g) {
  char buf[40];
  snprintf(buf, sizeof(buf),
           "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           (unsigned long)g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
           g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
  return buf;
}

static const char *adv_type_name(int t) {
  switch (t) {
    case 0: return "ConnectableUndirected";
    case 1: return "ConnectableDirected";
    case 2: return "ScannableUndirected";
    case 3: return "NonConnectableUndirected";
    case 4: return "ScanResponse";
    case 5: return "Extended";
    default: return "?";
  }
}

using AdvHandlerIface =
    wf::ITypedEventHandler<wda::BluetoothLEAdvertisementWatcher *,
                           wda::BluetoothLEAdvertisementReceivedEventArgs *>;

class AdvHandler : public HandlerBase<AdvHandlerIface> {
public:
  AdvHandler() : HandlerBase(__uuidof(AdvHandlerIface)) {}

  HRESULT STDMETHODCALLTYPE Invoke(
      wda::IBluetoothLEAdvertisementWatcher * /*sender*/,
      wda::IBluetoothLEAdvertisementReceivedEventArgs *args) override {
    // Counted before any filtering so we can tell "the radio reported nothing"
    // apart from "we discarded everything it reported".
    ++g_raw_events;
    if (!args) return S_OK;

    uint64_t addr = 0;
    args->get_BluetoothAddress(&addr);
    INT16 rssi = 0;
    args->get_RawSignalStrengthInDBm(&rssi);

    ComPtr<wda::IBluetoothLEAdvertisement> adv;
    if (FAILED(args->get_Advertisement(adv.put())) || !adv) return S_OK;

    // The local name may arrive in the scan response rather than the initial
    // advertisement, so it can legitimately be empty on some reports.
    HStr local_name;
    adv->get_LocalName(local_name.put());

    bool is_trezor = false;
    std::vector<GUID> uuid_list;
    ComPtr<wfc::IVector<GUID>> uuids;
    if (SUCCEEDED(adv->get_ServiceUuids(uuids.put())) && uuids) {
      unsigned size = 0;
      uuids->get_Size(&size);
      for (unsigned i = 0; i < size; ++i) {
        GUID g{};
        if (SUCCEEDED(uuids->GetAt(i, &g))) {
          uuid_list.push_back(g);
          if (IsEqualGUID(g, TREZOR_SERVICE)) is_trezor = true;
        }
      }
    }

    // Manufacturer-specific data carries the company id, which identifies the
    // vendor even when no service UUID is advertised.
    std::vector<uint16_t> mfg_ids;
    ComPtr<wfc::IVector<wda::BluetoothLEManufacturerData *>> mfg;
    if (SUCCEEDED(adv->get_ManufacturerData(mfg.put())) && mfg) {
      unsigned size = 0;
      mfg->get_Size(&size);
      for (unsigned i = 0; i < size; ++i) {
        ComPtr<wda::IBluetoothLEManufacturerData> item;
        if (SUCCEEDED(mfg->GetAt(i, item.put())) && item) {
          UINT16 company = 0;
          item->get_CompanyId(&company);
          mfg_ids.push_back(company);
        }
      }
    }

    int adv_type = -1;
    {
      wda::BluetoothLEAdvertisementType t;
      if (SUCCEEDED(args->get_AdvertisementType(&t))) adv_type = (int)t;
    }

    // The address type is authoritative here; guessing it from the address bits
    // works for resolvable private addresses but not in general.
    int address_type = 2;  // Unspecified
    {
      ComPtr<wda::IBluetoothLEAdvertisementReceivedEventArgs2> args2;
      if (SUCCEEDED(args->QueryInterface(
              __uuidof(wda::IBluetoothLEAdvertisementReceivedEventArgs2),
              args2.put_void())) &&
          args2) {
        wdb::BluetoothAddressType at;
        if (SUCCEEDED(args2->get_BluetoothAddressType(&at))) address_type = (int)at;
      }
    }

    // Raw AD structures, so nothing the device sends can hide from us.
    std::vector<std::string> sections;
    ComPtr<wfc::IVector<wda::BluetoothLEAdvertisementDataSection *>> secs;
    if (SUCCEEDED(adv->get_DataSections(secs.put())) && secs) {
      unsigned n = 0;
      secs->get_Size(&n);
      for (unsigned i = 0; i < n; ++i) {
        ComPtr<wda::IBluetoothLEAdvertisementDataSection> sec;
        if (FAILED(secs->GetAt(i, sec.put())) || !sec) continue;
        BYTE dtype = 0;
        sec->get_DataType(&dtype);
        ComPtr<wss::IBuffer> buf;
        if (FAILED(sec->get_Data(buf.put())) || !buf) continue;
        UINT32 blen = 0;
        buf->get_Length(&blen);
        ComPtr<Windows::Storage::Streams::IBufferByteAccess> acc;
        if (FAILED(buf.as(__uuidof(Windows::Storage::Streams::IBufferByteAccess), acc)) ||
            !acc) {
          continue;
        }
        byte *raw = nullptr;
        if (FAILED(acc->Buffer(&raw)) || !raw) continue;
        char head[16];
        snprintf(head, sizeof(head), "0x%02x ", (unsigned)dtype);
        std::string s(head);
        for (UINT32 b = 0; b < blen && b < 40; ++b) {
          char hx[4];
          snprintf(hx, sizeof(hx), "%02x", raw[b]);
          s += hx;
        }
        sections.push_back(std::move(s));
      }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    SeenDevice &d = seen_[addr];
    const bool was_trezor = d.has_trezor_service;
    d.address = addr;
    d.rssi = rssi;
    d.reports++;
    d.last_adv_type = adv_type;
    if (address_type != 2) d.addr_type = address_type;

    // Report devices as they appear rather than only at the end, so the device
    // can be put into pairing mode while the scan is already running.
    const std::string nm = local_name.to_utf8();
    std::string lname = nm;
    std::transform(lname.begin(), lname.end(), lname.begin(), ::tolower);
    const bool name_hit = lname.find("trezor") != std::string::npos;
    if (d.reports == 1) {
      printf("      + %s  rssi=%4d  %s\n", mac_to_string(addr).c_str(), rssi,
             nm.empty() ? "(no name)" : nm.c_str());
    }
    if ((is_trezor && !was_trezor) || name_hit) {
      printf("      *** TREZOR: %s  %s ***\n", mac_to_string(addr).c_str(),
             nm.empty() ? "(no name)" : nm.c_str());
    }
    // Keep the first non-empty name and never let a later empty report clear a
    // service match we already observed.
    const std::string n = local_name.to_utf8();
    if (!n.empty()) d.name = n;
    d.has_trezor_service = d.has_trezor_service || is_trezor;
    for (const GUID &g : uuid_list) {
      if (std::none_of(d.service_uuids.begin(), d.service_uuids.end(),
                       [&](const GUID &e) { return IsEqualGUID(e, g); })) {
        d.service_uuids.push_back(g);
      }
    }
    for (uint16_t id : mfg_ids) {
      if (std::find(d.manufacturer_ids.begin(), d.manufacturer_ids.end(), id) ==
          d.manufacturer_ids.end()) {
        d.manufacturer_ids.push_back(id);
      }
    }
    for (auto &s : sections) {
      if (std::find(d.raw_sections.begin(), d.raw_sections.end(), s) ==
          d.raw_sections.end()) {
        d.raw_sections.push_back(s);
      }
    }
    return S_OK;
  }

  bool found_trezor() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &kv : seen_) {
      if (kv.second.has_trezor_service) return true;
      std::string l = kv.second.name;
      std::transform(l.begin(), l.end(), l.begin(), ::tolower);
      if (l.find("trezor") != std::string::npos) return true;
    }
    return false;
  }

  std::map<uint64_t, SeenDevice> snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return seen_;
  }

private:
  std::mutex mutex_;
  std::map<uint64_t, SeenDevice> seen_;
};

// ---------------------------------------------------------------------------
// GATT notifications
// ---------------------------------------------------------------------------

using ValueHandlerIface =
    wf::ITypedEventHandler<wdg::GattCharacteristic *, wdg::GattValueChangedEventArgs *>;

class ValueHandler : public HandlerBase<ValueHandlerIface> {
public:
  ValueHandler() : HandlerBase(__uuidof(ValueHandlerIface)) {}

  HRESULT STDMETHODCALLTYPE Invoke(wdg::IGattCharacteristic * /*sender*/,
                                   wdg::IGattValueChangedEventArgs *args) override {
    if (!args) return S_OK;
    ComPtr<wss::IBuffer> buf;
    if (FAILED(args->get_CharacteristicValue(buf.put())) || !buf) return S_OK;

    UINT32 len = 0;
    buf->get_Length(&len);

    // IBufferByteAccess is the only way to reach the bytes behind an IBuffer.
    ComPtr<Windows::Storage::Streams::IBufferByteAccess> access;
    if (FAILED(buf.as(__uuidof(Windows::Storage::Streams::IBufferByteAccess), access)) ||
        !access) {
      return S_OK;
    }
    byte *raw = nullptr;
    if (FAILED(access->Buffer(&raw)) || !raw) return S_OK;

    std::vector<uint8_t> packet(raw, raw + len);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.push_back(std::move(packet));
    }
    cv_.notify_one();
    return S_OK;
  }

  // Returns an empty vector on timeout.
  std::vector<uint8_t> pop(unsigned timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                      [this] { return !queue_.empty(); })) {
      return {};
    }
    std::vector<uint8_t> out = std::move(queue_.front());
    queue_.pop_front();
    return out;
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::vector<uint8_t>> queue_;
};

// ---------------------------------------------------------------------------
// THP framing (only what the probe needs)
// ---------------------------------------------------------------------------

static uint32_t crc32_ieee(const uint8_t *data, size_t len) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    init = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

// Builds a THP channel allocation request, byte for byte identical to what
// hw::trezor::thp::write_message produces for Message::broadcast().
//
// Layout: ctrl(1) | channel(2 BE) | length(2 BE) | payload | crc32(4 BE)
// where `length` counts the payload plus the four checksum bytes.
static std::vector<uint8_t> build_channel_allocation_request(
    const std::vector<uint8_t> &nonce, size_t packet_size) {
  std::vector<uint8_t> msg;
  const size_t total = nonce.size() + 4;
  msg.push_back(0x40);                              // CHANNEL_ALLOCATION_REQ
  msg.push_back(0xFF);                              // BROADCAST_CHANNEL_ID hi
  msg.push_back(0xFF);                              // BROADCAST_CHANNEL_ID lo
  msg.push_back((uint8_t)((total >> 8) & 0xFF));
  msg.push_back((uint8_t)(total & 0xFF));
  msg.insert(msg.end(), nonce.begin(), nonce.end());

  const uint32_t crc = crc32_ieee(msg.data(), msg.size());
  msg.push_back((uint8_t)((crc >> 24) & 0xFF));
  msg.push_back((uint8_t)((crc >> 16) & 0xFF));
  msg.push_back((uint8_t)((crc >> 8) & 0xFF));
  msg.push_back((uint8_t)(crc & 0xFF));

  msg.resize(packet_size, 0);  // THP packets are always a full packet wide
  return msg;
}

// ---------------------------------------------------------------------------
// Reporting helpers
// ---------------------------------------------------------------------------

static std::string mac_to_string(uint64_t addr) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           (unsigned)((addr >> 40) & 0xFF), (unsigned)((addr >> 32) & 0xFF),
           (unsigned)((addr >> 24) & 0xFF), (unsigned)((addr >> 16) & 0xFF),
           (unsigned)((addr >> 8) & 0xFF), (unsigned)(addr & 0xFF));
  return buf;
}

static void hexdump(const std::vector<uint8_t> &v, size_t max_bytes = 32) {
  const size_t n = std::min(v.size(), max_bytes);
  for (size_t i = 0; i < n; ++i) printf("%02x", v[i]);
  if (v.size() > n) printf("... (%zu bytes total)", v.size());
}

static void fail(const char *step, HRESULT hr) {
  printf("  FAILED: %s (hr=0x%08lx)\n", step, (unsigned long)hr);
}

static const char *watcher_status_name(int s) {
  switch (s) {
    case 0: return "Created";
    case 1: return "Started";
    case 2: return "Stopping";
    case 3: return "Stopped";
    case 4: return "Aborted";
    default: return "?";
  }
}

struct ScanOutcome {
  std::map<uint64_t, SeenDevice> devices;
  int status = -1;
  bool started = false;
};

/**
 * Run one advertisement scan.
 *
 * `want_extended` and `want_coded` are requested separately because asking for
 * either on a radio that does not implement it makes Windows abort the watcher
 * instead of degrading -- a scan that then silently returns nothing at all. The
 * watcher status is reported so that case is visible rather than looking like
 * an empty room.
 */
static ScanOutcome run_scan(unsigned seconds, bool want_extended, bool want_coded) {
  ScanOutcome out;

  ComPtr<IActivationFactory> factory;
  HRESULT hr = get_factory(
      L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementWatcher",
      __uuidof(IActivationFactory), factory);
  if (FAILED(hr)) { fail("get watcher factory", hr); return out; }

  ComPtr<IInspectable> inspectable;
  hr = factory->ActivateInstance(inspectable.put());
  if (FAILED(hr)) { fail("activate watcher", hr); return out; }

  ComPtr<wda::IBluetoothLEAdvertisementWatcher> watcher;
  hr = inspectable.as(__uuidof(wda::IBluetoothLEAdvertisementWatcher), watcher);
  if (FAILED(hr)) { fail("QI IBluetoothLEAdvertisementWatcher", hr); return out; }

  // Active scanning solicits scan responses, where a peripheral commonly puts
  // its local name and any extra service UUIDs.
  watcher->put_ScanningMode(wda::BluetoothLEScanningMode_Active);

  if (want_extended) {
    ComPtr<wda::IBluetoothLEAdvertisementWatcher2> w2;
    if (SUCCEEDED(watcher.as(__uuidof(wda::IBluetoothLEAdvertisementWatcher2), w2)) && w2) {
      hr = w2->put_AllowExtendedAdvertisements(true);
      printf("    extended advertisements: %s\n", SUCCEEDED(hr) ? "on" : "rejected");
    } else {
      printf("    extended advertisements: unsupported on this Windows build\n");
    }
  }
  if (want_coded) {
    ComPtr<IBluetoothLEAdvertisementWatcher3_local> w3;
    if (SUCCEEDED(watcher.as(IID_IBluetoothLEAdvertisementWatcher3_local, w3)) && w3) {
      w3->put_UseUncoded1MPhy(true);
      hr = w3->put_UseCodedPhy(true);
      printf("    coded PHY scanning:      %s\n", SUCCEEDED(hr) ? "on" : "rejected");
    } else {
      printf("    coded PHY scanning:      unsupported on this Windows build\n");
    }
  }

  g_raw_events = 0;
  AdvHandler *collector = new AdvHandler();
  // Must be 90eb4eca-d465-5ea0-a61c-033c8c5ecef2; a mismatch would mean the
  // runtime can never QI our delegate and so can never invoke it.
  printf("    delegate IID: %s\n",
         guid_to_string(__uuidof(AdvHandlerIface)).c_str());
  EventRegistrationToken token{};
  hr = watcher->add_Received(collector, &token);
  if (FAILED(hr)) { fail("add_Received", hr); collector->Release(); return out; }

  hr = watcher->Start();
  if (FAILED(hr)) {
    fail("watcher Start", hr);
    watcher->remove_Received(token);
    collector->Release();
    return out;
  }
  out.started = true;

  // Poll the watcher while it runs: an aborted scan is the difference between
  // "nothing is out there" and "Windows refused the request".
  for (unsigned elapsed = 0; elapsed < seconds * 1000; elapsed += 250) {
    Sleep(250);
    wda::BluetoothLEAdvertisementWatcherStatus st;
    if (SUCCEEDED(watcher->get_Status(&st))) {
      out.status = (int)st;
      if ((int)st == 4) {  // Aborted
        printf("    watcher ABORTED after %ums\n", elapsed);
        break;
      }
    }
    // Stop as soon as a Trezor answers. The device advertises a rotating
    // private address, so the sooner we connect after hearing it the less
    // chance the address has already changed underneath us.
    if (collector->found_trezor()) {
      printf("    Trezor heard after %ums, connecting immediately\n", elapsed);
      break;
    }
  }

  watcher->Stop();
  watcher->remove_Received(token);
  printf("    raw callbacks fired: %ld\n", (long)g_raw_events);
  out.devices = collector->snapshot();
  collector->Release();
  return out;
}

/**
 * Enumerate Bluetooth LE association endpoints.
 *
 * This is an entirely different discovery mechanism from the advertisement
 * watcher: it goes through PnP device enumeration, which is what the Windows
 * Settings "Add a device" flow uses. Some drivers service one path and not the
 * other, so agreement between the two is strong evidence about where a fault
 * lies -- if this also comes back empty, the radio is not receiving anything
 * and no amount of application code will change that.
 */
static void aep_scan() {
  ComPtr<wde::IDeviceInformationStatics> statics;
  HRESULT hr = get_factory(L"Windows.Devices.Enumeration.DeviceInformation",
                           __uuidof(wde::IDeviceInformationStatics), statics);
  if (FAILED(hr)) {
    printf("    DeviceInformation API unavailable (hr=0x%08lx)\n", (unsigned long)hr);
    return;
  }

  // The Bluetooth LE association endpoint protocol id.
  HStr aqs(L"System.Devices.Aep.ProtocolId:=\"{bb7bb05e-5972-42b5-94fc-76eaa7084d49}\"");
  ComPtr<wf::IAsyncOperation<wde::DeviceInformationCollection *>> op;
  hr = statics->FindAllAsyncAqsFilter(aqs.get(), op.put());
  if (FAILED(hr)) { fail("FindAllAsyncAqsFilter", hr); return; }
  hr = await_op(op, 30000);
  if (FAILED(hr)) { fail("await AEP enumeration", hr); return; }

  // DeviceInformationCollection is a runtime class with no separate interface
  // of its own; its default interface is IVectorView<DeviceInformation*>, so the
  // result can be taken directly as that.
  ComPtr<wfc::IVectorView<wde::DeviceInformation *>> view;
  hr = op->GetResults(view.put());
  if (FAILED(hr) || !view) {
    printf("    no results (hr=0x%08lx)\n", (unsigned long)hr);
    return;
  }

  unsigned count = 0;
  view->get_Size(&count);
  printf("    %u BLE endpoint(s) known to Windows\n", count);
  for (unsigned i = 0; i < count; ++i) {
    ComPtr<wde::IDeviceInformation> info;
    if (FAILED(view->GetAt(i, info.put())) || !info) continue;
    HStr name, id;
    info->get_Name(name.put());
    info->get_Id(id.put());
    printf("      %-28s %s\n", name.to_utf8().c_str(), id.to_utf8().c_str());
  }
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
  unsigned scan_seconds = 8;
  if (argc > 1) scan_seconds = (unsigned)strtoul(argv[1], nullptr, 10);

  printf("Trezor BLE probe (WinRT)\n");
  printf("========================\n\n");

  // Multithreaded apartment: async completion and agile callbacks both work
  // without a message pump. RPC_E_CHANGED_MODE means someone already put this
  // thread in an STA, which our polling + agile handlers tolerate.
  HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    printf("FAILED: RoInitialize (hr=0x%08lx)\n", (unsigned long)hr);
    return 1;
  }

  // --- Step 0: what can this radio actually do? ---------------------------
  bool adapter_ext = false;  // radio understands BLE 5 advertising extensions
  printf("[0] Bluetooth adapter\n");
  {
    ComPtr<wdb::IBluetoothAdapterStatics> adapter_statics;
    HRESULT ahr = get_factory(L"Windows.Devices.Bluetooth.BluetoothAdapter",
                              __uuidof(wdb::IBluetoothAdapterStatics), adapter_statics);
    if (FAILED(ahr)) {
      printf("    no Bluetooth adapter API available\n");
    } else {
      ComPtr<wf::IAsyncOperation<wdb::BluetoothAdapter *>> aop;
      if (SUCCEEDED(adapter_statics->GetDefaultAsync(aop.put())) &&
          SUCCEEDED(await_op(aop, 5000))) {
        ComPtr<wdb::IBluetoothAdapter> adapter;
        if (SUCCEEDED(aop->GetResults(adapter.put())) && adapter) {
          boolean le = false, central = false;
          adapter->get_IsLowEnergySupported(&le);
          adapter->get_IsCentralRoleSupported(&central);
          printf("    low energy supported:    %s\n", le ? "yes" : "NO");
          printf("    central role supported:  %s\n", central ? "yes" : "NO");

          ComPtr<IBluetoothAdapter3_local> adapter3;
          if (SUCCEEDED(adapter.as(IID_IBluetoothAdapter3_local, adapter3)) && adapter3) {
            boolean ext = false;
            UINT32 maxlen = 0;
            adapter3->get_IsExtendedAdvertisingSupported(&ext);
            adapter3->get_MaxAdvertisementDataLength(&maxlen);
            adapter_ext = (ext != 0);
            printf("    extended advertising:    %s (max payload %u bytes)\n",
                   ext ? "yes" : "NO", (unsigned)maxlen);
            if (!ext) {
              printf("    NOTE: this radio predates BLE 5 advertising extensions.\n");
              printf("          A device advertising with them cannot be seen here.\n");
            }
          } else {
            printf("    extended advertising:    cannot query on this Windows build\n");
          }
        } else {
          printf("    no Bluetooth adapter present (is Bluetooth switched on?)\n");
        }
      } else {
        printf("    could not query the default adapter\n");
      }
    }
  }
  printf("\n");

  // A radio that is switched off still enumerates as a present adapter, and a
  // scan against it succeeds and simply reports nothing -- indistinguishable
  // from an empty room unless it is checked explicitly.
  printf("[0b] Radio state\n");
  {
    namespace wdr = ABI::Windows::Devices::Radios;
    ComPtr<wdr::IRadioStatics> radio_statics;
    HRESULT rhr = get_factory(L"Windows.Devices.Radios.Radio",
                              __uuidof(wdr::IRadioStatics), radio_statics);
    if (FAILED(rhr)) {
      printf("    radio API unavailable\n");
    } else {
      ComPtr<wf::IAsyncOperation<wfc::IVectorView<wdr::Radio *> *>> rop;
      if (SUCCEEDED(radio_statics->GetRadiosAsync(rop.put())) &&
          SUCCEEDED(await_op(rop, 10000))) {
        ComPtr<wfc::IVectorView<wdr::Radio *>> radios;
        if (SUCCEEDED(rop->GetResults(radios.put())) && radios) {
          unsigned rn = 0;
          radios->get_Size(&rn);
          for (unsigned i = 0; i < rn; ++i) {
            ComPtr<wdr::IRadio> r;
            if (FAILED(radios->GetAt(i, r.put())) || !r) continue;
            wdr::RadioKind kind;
            wdr::RadioState state;
            r->get_Kind(&kind);
            // mingw declares get_State as taking RadioState** where the actual
            // ABI takes a single out-pointer to the enum. The address is the
            // same either way; the cast just satisfies the wrong declaration.
            r->get_State(reinterpret_cast<wdr::RadioState **>(&state));
            if (kind != wdr::RadioKind_Bluetooth) continue;
            HStr rname;
            r->get_Name(rname.put());
            const char *sname = state == wdr::RadioState_On         ? "On"
                                : state == wdr::RadioState_Off      ? "OFF"
                                : state == wdr::RadioState_Disabled ? "DISABLED"
                                                                    : "unknown";
            printf("    bluetooth radio '%s': %s\n", rname.to_utf8().c_str(), sname);
          }
          if (!rn) printf("    no radios reported\n");
        }
      } else {
        printf("    could not enumerate radios\n");
      }
    }
  }
  printf("\n");

  // --- Step 1: scan advertisements ----------------------------------------
  printf("[1] Scanning advertisements for %u seconds...\n", scan_seconds);

  // Plain legacy scanning first: it is what every BLE radio implements, and
  // asking for anything more on hardware that lacks it aborts the whole scan.
  printf("    pass 1: legacy advertising\n");
  ScanOutcome scan = run_scan(scan_seconds, false, false);
  printf("    watcher ended in state %s, saw %zu device(s)\n",
         watcher_status_name(scan.status), scan.devices.size());

  // Only escalate if the plain scan came up short and the radio claims it can
  // do more. On this hardware that is usually not the case, which is itself the
  // answer.
  if (scan.devices.empty() && adapter_ext) {
    printf("\n    pass 2: BLE 5 extended advertising / coded PHY\n");
    ScanOutcome fancy = run_scan(scan_seconds, true, true);
    printf("    watcher ended in state %s, saw %zu device(s)\n",
           watcher_status_name(fancy.status), fancy.devices.size());
    if (!fancy.devices.empty()) scan = fancy;
  }

  auto seen = scan.devices;
  printf("\n    total: %zu distinct device(s)\n\n", seen.size());

  // Cross-check against PnP enumeration whenever the advertisement scan came up
  // empty, to tell an application-side fault from a radio that hears nothing.
  if (seen.empty()) {
    printf("[1b] Cross-check via Windows device enumeration...\n");
    aep_scan();
    printf("\n");
  }

  // Everything the radio saw is printed, not just service matches: if the
  // Trezor is present but advertising something other than we expect, it shows
  // up here and tells us what to match on instead.
  uint64_t target = 0;
  uint64_t name_match = 0;
  for (const auto &kv : seen) {
    const SeenDevice &d = kv.second;

    std::string lname = d.name;
    std::transform(lname.begin(), lname.end(), lname.begin(), ::tolower);
    const bool looks_like_trezor = lname.find("trezor") != std::string::npos;

    printf("    %s  rssi=%4d  reports=%-3d %-14s  %s%s\n",
           mac_to_string(d.address).c_str(), d.rssi, d.reports,
           adv_type_name(d.last_adv_type),
           d.name.empty() ? "(no name)" : d.name.c_str(),
           d.has_trezor_service  ? "   <-- TREZOR SERVICE"
           : looks_like_trezor   ? "   <-- NAME LOOKS LIKE TREZOR"
                                 : "");

    for (const GUID &g : d.service_uuids) {
      printf("            service %s\n", guid_to_string(g).c_str());
    }
    for (uint16_t id : d.manufacturer_ids) {
      printf("            manufacturer id 0x%04x\n", (unsigned)id);
    }
    for (const std::string &s : d.raw_sections) {
      printf("            raw %s\n", s.c_str());
    }

    if (d.has_trezor_service && !target) target = d.address;
    if (looks_like_trezor && !name_match) name_match = d.address;
  }
  printf("\n");

  // Falling back to the name lets the probe continue when the device does not
  // put its service UUID in the advertisement -- some peripherals only reveal
  // services after a connection is established.
  if (!target && name_match) {
    printf("No advertisement carried the Trezor service UUID, but a device is named\n");
    printf("like a Trezor. Continuing with %s to see whether the service appears\n",
           mac_to_string(name_match).c_str());
    printf("after connecting.\n\n");
    target = name_match;
  }

  if (!target) {
    printf("Nothing matched the Trezor service %s\n",
           "8c000001-a59b-4d58-a9ad-073df69fa1b1");
    printf("and no device advertised a Trezor-like name.\n\n");
    if (seen.empty()) {
      printf("The scan saw NO BLE devices at all. That points at the Windows side:\n");
      printf("  - Bluetooth turned off, or the adapter is Classic-only (no BLE)\n");
      printf("  - Settings > Privacy & security > Bluetooth devices: allow desktop apps\n");
    } else {
      printf("The scan works (%zu other devices seen), so the Trezor is either not\n",
             seen.size());
      printf("advertising, out of range, or already connected to something else.\n");
      printf("Disconnect it from any phone or Trezor Suite session and retry.\n");
    }
    printf("\nPress Enter to close.\n");
    getchar();
    return 2;
  }

  printf("[2] Connecting to %s (no OS pairing required)...\n",
         mac_to_string(target).c_str());

  ComPtr<wdb::IBluetoothLEDeviceStatics> le_statics;
  hr = get_factory(L"Windows.Devices.Bluetooth.BluetoothLEDevice",
                   __uuidof(wdb::IBluetoothLEDeviceStatics), le_statics);
  if (FAILED(hr)) { fail("get BluetoothLEDevice statics", hr); return 1; }

  // Look up the address type we actually observed. Connecting to a random
  // address as though it were public silently yields an unreachable device.
  int target_addr_type = 2;
  {
    auto it = seen.find(target);
    if (it != seen.end()) target_addr_type = it->second.addr_type;
  }
  printf("    address type: %s\n", target_addr_type == 0   ? "Public"
                                   : target_addr_type == 1 ? "Random (rotates)"
                                                           : "Unspecified");

  ComPtr<wf::IAsyncOperation<wdb::BluetoothLEDevice *>> dev_op;
  ComPtr<IBluetoothLEDeviceStatics2_local> le_statics2;
  if (target_addr_type != 2 &&
      SUCCEEDED(le_statics.as(IID_IBluetoothLEDeviceStatics2_local, le_statics2)) &&
      le_statics2) {
    hr = le_statics2->FromBluetoothAddressWithBluetoothAddressTypeAsync(
        target, target_addr_type, dev_op.put());
  } else {
    hr = le_statics->FromBluetoothAddressAsync(target, dev_op.put());
  }
  if (FAILED(hr)) { fail("FromBluetoothAddressAsync", hr); return 1; }
  hr = await_op(dev_op);
  if (FAILED(hr)) { fail("await FromBluetoothAddressAsync", hr); return 1; }

  ComPtr<wdb::IBluetoothLEDevice> device;
  hr = dev_op->GetResults(device.put());
  if (FAILED(hr) || !device) { fail("GetResults(BluetoothLEDevice)", hr); return 1; }
  printf("    got device object\n\n");

  // --- Step 3: discover the Trezor service --------------------------------
  printf("[3] Discovering GATT service...\n");

  ComPtr<IBluetoothLEDevice3_local> device3;
  hr = device.as(IID_IBluetoothLEDevice3_local, device3);
  if (FAILED(hr) || !device3) { fail("QI IBluetoothLEDevice3", hr); return 1; }

  // Uncached forces a real over-the-air discovery instead of trusting whatever
  // Windows may have remembered. This is also what triggers the connection.
  ComPtr<wf::IAsyncOperation<wdg::GattDeviceServicesResult *>> svc_op;
  hr = device3->GetGattServicesForUuidWithCacheModeAsync(
      TREZOR_SERVICE, wdb::BluetoothCacheMode_Uncached, svc_op.put());
  if (FAILED(hr)) { fail("GetGattServicesForUuidAsync", hr); return 1; }
  hr = await_op(svc_op);
  if (FAILED(hr)) { fail("await service discovery", hr); return 1; }

  ComPtr<wdg::IGattDeviceServicesResult> svc_result;
  hr = svc_op->GetResults(svc_result.put());
  if (FAILED(hr) || !svc_result) { fail("GetResults(services)", hr); return 1; }

  wdg::GattCommunicationStatus comm = wdg::GattCommunicationStatus_Unreachable;
  svc_result->get_Status(&comm);
  if (comm != wdg::GattCommunicationStatus_Success) {
    printf("    FAILED: service discovery status=%d "
           "(0=Success 1=Unreachable 2=ProtocolError 3=AccessDenied)\n", (int)comm);
    return 1;
  }

  ComPtr<wfc::IVectorView<wdg::GattDeviceService *>> services;
  svc_result->get_Services(services.put());
  unsigned svc_count = 0;
  if (services) services->get_Size(&svc_count);

  if (!svc_count) {
    // The Trezor service was not found. List whatever the device does expose,
    // which tells us whether we reached the right device at all.
    printf("    Trezor service not present. Enumerating all services...\n");
    ComPtr<wf::IAsyncOperation<wdg::GattDeviceServicesResult *>> all_op;
    if (SUCCEEDED(device3->GetGattServicesWithCacheModeAsync(
            wdb::BluetoothCacheMode_Uncached, all_op.put())) &&
        SUCCEEDED(await_op(all_op))) {
      ComPtr<wdg::IGattDeviceServicesResult> all_res;
      if (SUCCEEDED(all_op->GetResults(all_res.put())) && all_res) {
        ComPtr<wfc::IVectorView<wdg::GattDeviceService *>> all_services;
        all_res->get_Services(all_services.put());
        unsigned n = 0;
        if (all_services) all_services->get_Size(&n);
        printf("    device exposes %u service(s):\n", n);
        for (unsigned i = 0; i < n; ++i) {
          ComPtr<wdg::IGattDeviceService> s;
          if (SUCCEEDED(all_services->GetAt(i, s.put())) && s) {
            GUID g{};
            s->get_Uuid(&g);
            printf("      %s\n", guid_to_string(g).c_str());
          }
        }
      }
    }
    return 1;
  }

  ComPtr<wdg::IGattDeviceService> service;
  services->GetAt(0, service.put());
  printf("    found Trezor service\n\n");

  // --- Step 4: find the RX/TX characteristics -----------------------------
  printf("[4] Resolving characteristics...\n");

  ComPtr<wdg::IGattDeviceService3> service3;
  hr = service.as(__uuidof(wdg::IGattDeviceService3), service3);
  if (FAILED(hr) || !service3) { fail("QI IGattDeviceService3", hr); return 1; }

  auto find_char = [&](const GUID &uuid, const char *label,
                       ComPtr<wdg::IGattCharacteristic> &out) -> bool {
    ComPtr<wf::IAsyncOperation<wdg::GattCharacteristicsResult *>> op;
    HRESULT h = service3->GetCharacteristicsForUuidWithCacheModeAsync(
        uuid, wdb::BluetoothCacheMode_Uncached, op.put());
    if (FAILED(h)) { fail(label, h); return false; }
    h = await_op(op);
    if (FAILED(h)) { fail(label, h); return false; }

    ComPtr<wdg::IGattCharacteristicsResult> res;
    if (FAILED(op->GetResults(res.put())) || !res) { fail(label, E_FAIL); return false; }
    wdg::GattCommunicationStatus st = wdg::GattCommunicationStatus_Unreachable;
    res->get_Status(&st);
    if (st != wdg::GattCommunicationStatus_Success) {
      printf("    FAILED: %s status=%d\n", label, (int)st);
      return false;
    }
    ComPtr<wfc::IVectorView<wdg::GattCharacteristic *>> vec;
    res->get_Characteristics(vec.put());
    unsigned n = 0;
    if (vec) vec->get_Size(&n);
    if (!n) { printf("    FAILED: %s not found\n", label); return false; }
    vec->GetAt(0, out.put());
    printf("    %s ok\n", label);
    return true;
  };

  ComPtr<wdg::IGattCharacteristic> rx, tx;
  if (!find_char(TREZOR_CHAR_RX, "RX (host writes)", rx)) return 1;
  if (!find_char(TREZOR_CHAR_TX, "TX (device notifies)", tx)) return 1;

  // The negotiated MTU determines the THP packet size. Trezor uses 244 bytes of
  // payload, which needs an ATT MTU of at least 247 (3 bytes of ATT overhead).
  size_t packet_size = 244;
  ComPtr<wdg::IGattSession> session;
  if (SUCCEEDED(service3->get_Session(session.put())) && session) {
    UINT16 mtu = 0;
    if (SUCCEEDED(session->get_MaxPduSize(&mtu)) && mtu > 3) {
      printf("    negotiated ATT MTU: %u\n", (unsigned)mtu);
      packet_size = std::min<size_t>(244, (size_t)mtu - 3);
    }
  }
  printf("    using THP packet size: %zu\n\n", packet_size);

  // --- Step 4b: bond ------------------------------------------------------
  // The Trezor will not enable notifications over an unauthenticated link, so
  // the link has to be bonded before the CCCD write. Connect first, pair second
  // -- the same order trezorlib uses.
  printf("[4b] Pairing...\n");
  {
    // IBluetoothLEDevice2 (which carries the DeviceInformation property) is not
    // defined by mingw either, so resolve it from the device id instead - that
    // only needs statics mingw does provide.
    ComPtr<wde::IDeviceInformation> devinfo;
    HStr devid;
    device->get_DeviceId(devid.put());
    ComPtr<wde::IDeviceInformationStatics> di_statics;
    if (SUCCEEDED(get_factory(L"Windows.Devices.Enumeration.DeviceInformation",
                              __uuidof(wde::IDeviceInformationStatics), di_statics))) {
      ComPtr<wf::IAsyncOperation<wde::DeviceInformation *>> diop;
      if (SUCCEEDED(di_statics->CreateFromIdAsync(devid.get(), diop.put())) &&
          SUCCEEDED(await_op(diop, 15000))) {
        diop->GetResults(devinfo.put());
      }
    }
    if (devinfo) {
      ComPtr<IDeviceInformation2_local> devinfo2;
      if (SUCCEEDED(devinfo.as(IID_IDeviceInformation2_local, devinfo2)) && devinfo2) {
        ComPtr<IDeviceInformationPairing_local> pairing;
        if (SUCCEEDED(devinfo2->get_Pairing(pairing.put())) && pairing) {
          boolean is_paired = false, can_pair = false;
          pairing->get_IsPaired(&is_paired);
          pairing->get_CanPair(&can_pair);
          printf("    already paired: %s, can pair: %s\n", is_paired ? "yes" : "no",
                 can_pair ? "yes" : "no");

          if (!is_paired) {
            ComPtr<IDeviceInformationPairing2_local> pairing2;
            ComPtr<IDeviceInformationCustomPairing_local> custom;
            if (SUCCEEDED(pairing.as(IID_IDeviceInformationPairing2_local, pairing2)) &&
                pairing2 && SUCCEEDED(pairing2->get_Custom(custom.put())) && custom) {
              printf("    custom pairing (confirm on the Trezor if it asks)...\n");

              PairingRequestedHandler *ph = new PairingRequestedHandler();
              EventRegistrationToken ptok{};
              HRESULT ahr = custom->add_PairingRequested(ph, &ptok);
              if (FAILED(ahr)) {
                printf("    add_PairingRequested failed (hr=0x%08lx)\n", (unsigned long)ahr);
              }

              // Declare every ceremony the device might choose; Windows picks
              // one it and the peripheral both support.
              const int kinds = kPairingKindConfirmOnly | kPairingKindDisplayPin |
                                kPairingKindConfirmPinMatch;
              ComPtr<IAsyncOperationDevicePairingResult_local> pop;
              // Encryption is required: the whole point is to satisfy the
              // device's demand for an authenticated link.
              HRESULT phr = custom->PairWithProtectionLevelAsync(kinds, 2 /*Encryption*/,
                                                                 pop.put());
              if (SUCCEEDED(phr) && SUCCEEDED(await_op(pop, 90000))) {
                ComPtr<IDevicePairingResult_local> pres;
                if (SUCCEEDED(pop->GetResults(pres.put())) && pres) {
                  int st = -1;
                  pres->get_Status(&st);
                  printf("    pairing result: %s (%d)\n", pairing_status_name(st), st);
                }
              } else {
                printf("    custom PairAsync failed (hr=0x%08lx)\n", (unsigned long)phr);
              }
              custom->remove_PairingRequested(ptok);
              ph->Release();
            } else {
              printf("    custom pairing unavailable, trying default ceremony\n");
              ComPtr<IAsyncOperationDevicePairingResult_local> pop;
              if (SUCCEEDED(pairing->PairAsync(pop.put())) &&
                  SUCCEEDED(await_op(pop, 60000))) {
                ComPtr<IDevicePairingResult_local> pres;
                if (SUCCEEDED(pop->GetResults(pres.put())) && pres) {
                  int st = -1;
                  pres->get_Status(&st);
                  printf("    pairing result: %s (%d)\n", pairing_status_name(st), st);
                }
              }
            }
          }
        } else {
          printf("    no pairing interface\n");
        }
      } else {
        printf("    IDeviceInformation2 unavailable\n");
      }
    } else {
      printf("    no device information\n");
    }
  }
  printf("\n");

  // --- Step 5: subscribe --------------------------------------------------
  printf("[5] Subscribing to notifications...\n");

  ValueHandler *value_handler = new ValueHandler();
  EventRegistrationToken value_token{};
  hr = tx->add_ValueChanged(value_handler, &value_token);
  if (FAILED(hr)) { fail("add_ValueChanged", hr); value_handler->Release(); return 1; }

  ComPtr<wf::IAsyncOperation<wdg::GattCommunicationStatus>> cccd_op;
  hr = tx->WriteClientCharacteristicConfigurationDescriptorAsync(
      wdg::GattClientCharacteristicConfigurationDescriptorValue_Notify, cccd_op.put());
  if (FAILED(hr)) { fail("write CCCD", hr); value_handler->Release(); return 1; }
  hr = await_op(cccd_op);
  if (FAILED(hr)) { fail("await CCCD write", hr); value_handler->Release(); return 1; }

  wdg::GattCommunicationStatus cccd_status = wdg::GattCommunicationStatus_Unreachable;
  cccd_op->GetResults(&cccd_status);
  if (cccd_status != wdg::GattCommunicationStatus_Success) {
    printf("    FAILED: CCCD write status=%d\n", (int)cccd_status);
    value_handler->Release();
    return 1;
  }
  printf("    subscribed\n\n");

  // --- Step 6: THP round trip ---------------------------------------------
  printf("[6] THP channel allocation round trip...\n");

  std::vector<uint8_t> nonce(8);
  for (auto &b : nonce) b = (uint8_t)(rand() & 0xFF);
  const std::vector<uint8_t> request = build_channel_allocation_request(nonce, packet_size);

  // Build an IBuffer for the write. DataWriter is the sanctioned way to turn
  // raw bytes into one.
  ComPtr<IActivationFactory> dw_factory;
  hr = get_factory(L"Windows.Storage.Streams.DataWriter", __uuidof(IActivationFactory),
                   dw_factory);
  if (FAILED(hr)) { fail("DataWriter factory", hr); value_handler->Release(); return 1; }
  ComPtr<IInspectable> dw_inspectable;
  dw_factory->ActivateInstance(dw_inspectable.put());
  ComPtr<wss::IDataWriter> writer;
  hr = dw_inspectable.as(__uuidof(wss::IDataWriter), writer);
  if (FAILED(hr)) { fail("QI IDataWriter", hr); value_handler->Release(); return 1; }
  writer->WriteBytes((UINT32)request.size(), const_cast<BYTE *>(request.data()));
  ComPtr<wss::IBuffer> buffer;
  hr = writer->DetachBuffer(buffer.put());
  if (FAILED(hr)) { fail("DetachBuffer", hr); value_handler->Release(); return 1; }

  ComPtr<wdg::IGattCharacteristic3> rx3;
  hr = rx.as(__uuidof(wdg::IGattCharacteristic3), rx3);
  if (FAILED(hr) || !rx3) { fail("QI IGattCharacteristic3", hr); value_handler->Release(); return 1; }

  // Trezor's RX characteristic is write-without-response, matching the fire and
  // forget framing THP expects.
  ComPtr<wf::IAsyncOperation<wdg::GattWriteResult *>> write_op;
  hr = rx3->WriteValueWithResultAndOptionAsync(
      buffer.get(), wdg::GattWriteOption_WriteWithoutResponse, write_op.put());
  if (FAILED(hr)) { fail("WriteValueWithResultAndOptionAsync", hr); value_handler->Release(); return 1; }
  hr = await_op(write_op);
  if (FAILED(hr)) { fail("await write", hr); value_handler->Release(); return 1; }

  ComPtr<wdg::IGattWriteResult> write_res;
  write_op->GetResults(write_res.put());
  if (write_res) {
    wdg::GattCommunicationStatus ws = wdg::GattCommunicationStatus_Unreachable;
    write_res->get_Status(&ws);
    if (ws != wdg::GattCommunicationStatus_Success) {
      printf("    FAILED: write status=%d\n", (int)ws);
      value_handler->Release();
      return 1;
    }
  }
  printf("    sent  ");
  hexdump(std::vector<uint8_t>(request.begin(), request.begin() + 17));
  printf("\n");

  const std::vector<uint8_t> reply = value_handler->pop(5000);
  if (reply.empty()) {
    printf("    FAILED: no notification within 5s\n");
    value_handler->Release();
    printf("\nPress Enter to close.\n");
    getchar();
    return 1;
  }
  printf("    recv  ");
  hexdump(reply);
  printf("\n");

  // A channel allocation response echoes our nonce back in the payload.
  const bool ok = reply.size() >= 13 && (reply[0] & 0xDF) == 0x41 &&
                  std::equal(nonce.begin(), nonce.end(), reply.begin() + 5);
  printf("\n----------------------------------------\n");
  if (ok) {
    printf("SUCCESS: full THP round trip over BLE.\n");
    printf("Address to use: %s\n", mac_to_string(target).c_str());
  } else {
    printf("Device replied, but not with a valid channel allocation response.\n");
    printf("ctrl=0x%02x (expected 0x41), nonce echo %s\n", reply[0],
           reply.size() >= 13 && std::equal(nonce.begin(), nonce.end(), reply.begin() + 5)
               ? "ok" : "MISMATCH");
  }

  tx->remove_ValueChanged(value_token);
  value_handler->Release();

  printf("\nPress Enter to close.\n");
  getchar();
  return ok ? 0 : 1;
}
