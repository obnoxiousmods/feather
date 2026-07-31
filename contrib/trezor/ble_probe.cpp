// Standalone diagnostic: does the Win32 GATT path see the Trezor over BLE?
//
// Runs the same enumeration the Feather backend does, but prints every step so
// we can tell "no bonded BLE devices" from "bonded but no Trezor service" from
// "found it but cannot open a handle". Self-contained; no monero dependencies.
//
// Build: x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -o ble-probe.exe \
//            ble_probe.cpp -lbluetoothapis -lsetupapi

#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <bthdef.h>
#include <bthledef.h>
#include <bluetoothapis.h>
#include <bluetoothleapis.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static const GUID TREZOR_SERVICE_GUID =
    {0x8c000001, 0xa59b, 0x4d58, {0xa9, 0xad, 0x07, 0x3d, 0xf6, 0x9f, 0xa1, 0xb1}};
static const GUID TREZOR_RX_GUID =
    {0x8c000002, 0xa59b, 0x4d58, {0xa9, 0xad, 0x07, 0x3d, 0xf6, 0x9f, 0xa1, 0xb1}};
static const GUID TREZOR_TX_GUID =
    {0x8c000003, 0xa59b, 0x4d58, {0xa9, 0xad, 0x07, 0x3d, 0xf6, 0x9f, 0xa1, 0xb1}};

static std::string narrow(const std::wstring &w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}

static void print_uuid(const BTH_LE_UUID &u) {
  if (u.IsShortUuid) { printf("short:0x%04x", u.Value.ShortUuid); return; }
  const GUID &g = u.Value.LongUuid;
  printf("%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
         (unsigned long)g.Data1, g.Data2, g.Data3,
         g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5],
         g.Data4[6], g.Data4[7]);
}

static bool same(const BTH_LE_UUID &u, const GUID &g) {
  return !u.IsShortUuid && IsEqualGUID(u.Value.LongUuid, g);
}

int main() {
  printf("Trezor BLE probe\n================\n\n");

  HDEVINFO set = SetupDiGetClassDevsW(&GUID_BLUETOOTHLE_DEVICE_INTERFACE, nullptr, nullptr,
                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE) {
    printf("FAIL: SetupDiGetClassDevs failed (err=%lu)\n", GetLastError());
    printf("      Bluetooth may be disabled or unsupported on this machine.\n");
    return 1;
  }

  SP_DEVICE_INTERFACE_DATA ifd{};
  ifd.cbSize = sizeof(ifd);
  int total = 0, trezors = 0;

  for (DWORD i = 0;
       SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_BLUETOOTHLE_DEVICE_INTERFACE, i, &ifd);
       ++i) {
    ++total;
    DWORD needed = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &needed, nullptr);
    if (!needed) continue;

    std::vector<uint8_t> buf(needed);
    auto *detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    SP_DEVINFO_DATA devinfo{};
    devinfo.cbSize = sizeof(devinfo);
    if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, needed, nullptr, &devinfo)) continue;

    WCHAR namebuf[256] = {};
    if (!SetupDiGetDeviceRegistryPropertyW(set, &devinfo, SPDRP_FRIENDLYNAME, nullptr,
                                           (PBYTE)namebuf, sizeof(namebuf), nullptr)) {
      SetupDiGetDeviceRegistryPropertyW(set, &devinfo, SPDRP_DEVICEDESC, nullptr,
                                        (PBYTE)namebuf, sizeof(namebuf), nullptr);
    }
    printf("[%d] %s\n", total, narrow(namebuf).c_str());

    HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      printf("     cannot open handle (err=%lu) - skipping\n\n", GetLastError());
      continue;
    }

    USHORT count = 0;
    BluetoothGATTGetServices(h, 0, nullptr, &count, BLUETOOTH_GATT_FLAG_NONE);
    if (!count) {
      printf("     no GATT services reported\n\n");
      CloseHandle(h);
      continue;
    }
    std::vector<BTH_LE_GATT_SERVICE> svcs(count);
    if (FAILED(BluetoothGATTGetServices(h, count, svcs.data(), &count, BLUETOOTH_GATT_FLAG_NONE))) {
      printf("     could not read services\n\n");
      CloseHandle(h);
      continue;
    }

    bool is_trezor = false;
    for (auto &s : svcs) {
      printf("     service ");
      print_uuid(s.ServiceUuid);
      if (same(s.ServiceUuid, TREZOR_SERVICE_GUID)) { printf("   <-- TREZOR"); is_trezor = true; }
      printf("\n");
    }

    if (is_trezor) {
      ++trezors;
      for (auto &s : svcs) {
        if (!same(s.ServiceUuid, TREZOR_SERVICE_GUID)) continue;
        USHORT cc = 0;
        BluetoothGATTGetCharacteristics(h, &s, 0, nullptr, &cc, BLUETOOTH_GATT_FLAG_NONE);
        std::vector<BTH_LE_GATT_CHARACTERISTIC> chars(cc);
        if (cc && SUCCEEDED(BluetoothGATTGetCharacteristics(h, &s, cc, chars.data(), &cc,
                                                           BLUETOOTH_GATT_FLAG_NONE))) {
          for (auto &c : chars) {
            printf("       char ");
            print_uuid(c.CharacteristicUuid);
            if (same(c.CharacteristicUuid, TREZOR_RX_GUID)) printf("   <-- RX (host writes)");
            if (same(c.CharacteristicUuid, TREZOR_TX_GUID)) printf("   <-- TX (notifies)");
            printf("  [notify=%d write=%d wwr=%d]\n", c.IsNotifiable, c.IsWritable,
                   c.IsWritableWithoutResponse);
          }
        }
      }
      printf("     RESULT: usable Trezor over BLE\n");
    }
    printf("\n");
    CloseHandle(h);
  }

  SetupDiDestroyDeviceInfoList(set);

  printf("----------------------------------------\n");
  printf("Bonded BLE device interfaces: %d\n", total);
  printf("Trezors exposing the service: %d\n", trezors);
  if (!total) {
    printf("\nNo bonded BLE devices at all. Pair the Trezor in\n"
           "Settings > Bluetooth & devices, and enable Bluetooth on the device.\n");
  } else if (!trezors) {
    printf("\nBLE devices are bonded, but none exposes the Trezor service.\n"
           "Check that Bluetooth is enabled on the Trezor and that it is paired.\n");
  }
  printf("\nPress Enter to close.\n");
  getchar();
  return 0;
}
