// luzvda-ctl: installs LuzVDA, removes SudoVDA, adds, inspects and removes displays by hand, and
// measures how fast a display's frames arrive (capture-test).
// Run it from an elevated prompt: the driver opens only for SYSTEM and Administrators.
//
// Builds with MSVC and MinGW:
//   cl /std:c++17 /EHsc /O2 /I..\..\include luzvda-ctl.cpp setupapi.lib newdev.lib dxgi.lib d3d11.lib ole32.lib user32.lib advapi32.lib
//   x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -I../../include luzvda-ctl.cpp -lsetupapi -lnewdev -ldxgi -ld3d11 -lole32 -luser32 -ladvapi32
//
// SPDX-License-Identifier: MS-PL

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <newdev.h>
#include <dxgi1_6.h>
#include <conio.h>

#include <luzvda/luzvda-ioctl.h>
#include <luzvda/luzvda-frames.h>

#include "frame_code.h"

// MSVC's setupapi.h has it; MinGW's doesn't
#ifndef MAX_CLASS_NAME_LEN
#define MAX_CLASS_NAME_LEN 32
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static const GUID s_InterfaceGuid = LUZVDA_INTERFACE_GUID_INIT;
static const wchar_t s_HardwareId[] = L"Root\\LuzVDA";
static const wchar_t s_LegacyHardwareId[] = L"Root\\SudoMaker\\SudoVDA";

static void print_error(const char *what, DWORD error = GetLastError()) {
  wchar_t *message = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, (LPWSTR) &message, 0, nullptr);
  std::fprintf(stderr, "%s failed: %lu %ls", what, error, message ? message : L"\n");
  LocalFree(message);
}

static std::string guid_string(const LUZVDA_GUID &g) {
  GUID guid;
  std::memcpy(&guid, g.bytes, sizeof(guid));
  char out[64];
  std::snprintf(out, sizeof(out), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", (unsigned long) guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
  return out;
}

static bool parse_guid(const char *text, LUZVDA_GUID &out) {
  std::wstring w = L"{";
  for (const char *p = text; *p; ++p) {
    if (*p != '{' && *p != '}') {
      w.push_back((wchar_t) *p);
    }
  }
  w.push_back(L'}');
  GUID guid;
  if (FAILED(CLSIDFromString(w.c_str(), &guid))) {
    return false;
  }
  std::memcpy(out.bytes, &guid, sizeof(guid));
  return true;
}

// WxH@Hz, the rate with decimals: 3024x1964@120, 1920x1080@59.94
static bool parse_mode(const char *text, LUZVDA_MODE &out) {
  unsigned w = 0, h = 0;
  double hz = 0;
  if (std::sscanf(text, "%ux%u@%lf", &w, &h, &hz) != 3 || hz <= 0) {
    return false;
  }
  out = {w, h, (uint32_t) std::lround(hz * 1000)};
  return true;
}

#pragma region Device

// The device for reading and writing; error is why not, taken before cleanup calls overwrite it
static HANDLE open_device(DWORD &error) {
  error = ERROR_FILE_NOT_FOUND;
  HDEVINFO set = SetupDiGetClassDevsW(&s_InterfaceGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE) {
    error = GetLastError();
    return INVALID_HANDLE_VALUE;
  }

  HANDLE handle = INVALID_HANDLE_VALUE;
  SP_DEVICE_INTERFACE_DATA iface = {};
  iface.cbSize = sizeof(iface);
  for (DWORD i = 0; handle == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &s_InterfaceGuid, i, &iface); ++i) {
    DWORD size = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &size, nullptr);
    if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
      continue;
    }
    std::vector<BYTE> buffer(size);
    auto *detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *) buffer.data();
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, &size, nullptr)) {
      handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
      error = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    }
  }

  SetupDiDestroyDeviceInfoList(set);
  return handle;
}

static bool request(HANDLE device, DWORD code, const void *in, DWORD in_size, void *out, DWORD out_size, const char *what) {
  DWORD returned = 0;
  if (!DeviceIoControl(device, code, (void *) in, in_size, out, out_size, &returned, nullptr)) {
    print_error(what);
    return false;
  }
  return true;
}

struct device_t {
  DWORD error = ERROR_SUCCESS;
  HANDLE handle = open_device(error);

  ~device_t() {
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }

  bool ok() const {
    if (handle == INVALID_HANDLE_VALUE) {
      print_error("Opening LuzVDA (installed? elevated prompt?)", error);
      return false;
    }
    return true;
  }
};

static bool list_displays(HANDLE device, LUZVDA_LIST &list) {
  std::memset(&list, 0, sizeof(list));
  return request(device, IOCTL_LUZVDA_LIST_DISPLAYS, nullptr, 0, &list, sizeof(list), "Listing displays");
}

#pragma endregion

#pragma region Windows display configuration

struct active_display_t {
  DISPLAYCONFIG_PATH_INFO path;
  DISPLAYCONFIG_SOURCE_MODE source;
  std::wstring gdi_name;
};

static bool query_config(std::vector<DISPLAYCONFIG_PATH_INFO> &paths, std::vector<DISPLAYCONFIG_MODE_INFO> &modes) {
  UINT32 path_count = 0, mode_count = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
    return false;
  }
  paths.resize(path_count);
  modes.resize(mode_count);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) != ERROR_SUCCESS) {
    return false;
  }
  paths.resize(path_count);
  modes.resize(mode_count);
  return true;
}

static bool same_luid(const LUID &a, const LUZVDA_LUID &b) {
  return a.LowPart == b.low && a.HighPart == b.high;
}

// The active path for one of the driver's displays
static bool find_active(const LUZVDA_LIST_ENTRY &entry, active_display_t &out, std::vector<DISPLAYCONFIG_PATH_INFO> *all_paths = nullptr, std::vector<DISPLAYCONFIG_MODE_INFO> *all_modes = nullptr, size_t *path_index = nullptr) {
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  if (!query_config(paths, modes)) {
    return false;
  }

  for (size_t i = 0; i < paths.size(); ++i) {
    const auto &p = paths[i];
    if (!same_luid(p.targetInfo.adapterId, entry.adapter) || p.targetInfo.id != entry.target_id) {
      continue;
    }

    DISPLAYCONFIG_SOURCE_DEVICE_NAME name = {};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    name.header.size = sizeof(name);
    name.header.adapterId = p.sourceInfo.adapterId;
    name.header.id = p.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) {
      return false;
    }

    out.path = p;
    out.gdi_name = name.viewGdiDeviceName;
    out.source = {};
    for (const auto &m : modes) {
      if (m.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE && m.id == p.sourceInfo.id && m.adapterId.LowPart == p.sourceInfo.adapterId.LowPart && m.adapterId.HighPart == p.sourceInfo.adapterId.HighPart) {
        out.source = m.sourceMode;
      }
    }

    if (all_paths) {
      *all_paths = paths;
      *all_modes = modes;
      *path_index = i;
    }
    return true;
  }
  return false;
}

static bool find_entry(HANDLE device, const LUZVDA_GUID &guid, LUZVDA_LIST_ENTRY &out) {
  LUZVDA_LIST list;
  if (!list_displays(device, list)) {
    return false;
  }
  for (uint32_t i = 0; i < list.count; ++i) {
    if (std::memcmp(list.displays[i].monitor_guid.bytes, guid.bytes, 16) == 0) {
      out = list.displays[i];
      return true;
    }
  }
  std::fprintf(stderr, "No LuzVDA display has that GUID\n");
  return false;
}

static double rate_hz(const DISPLAYCONFIG_RATIONAL &r) {
  return r.Denominator ? (double) r.Numerator / r.Denominator : 0;
}

static void print_display(const LUZVDA_LIST_ENTRY &entry) {
  std::printf("Display %s\n", guid_string(entry.monitor_guid).c_str());
  std::printf("  connector %u, target %u, adapter %08lx:%08lx, preferred %ux%u at %.3f Hz\n", entry.connector, entry.target_id, (unsigned long) entry.adapter.high, (unsigned long) entry.adapter.low, entry.preferred_mode.width, entry.preferred_mode.height, entry.preferred_mode.refresh_mhz / 1000.0);

  active_display_t active;
  if (!find_active(entry, active)) {
    std::printf("  not active in Windows' display configuration\n");
    return;
  }

  std::printf("  %ls: %ux%u at %.3f Hz, position %ld,%ld\n", active.gdi_name.c_str(), active.source.width, active.source.height, rate_hz(active.path.targetInfo.refreshRate), active.source.position.x, active.source.position.y);

  DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
  target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
  target.header.size = sizeof(target);
  target.header.adapterId = active.path.targetInfo.adapterId;
  target.header.id = active.path.targetInfo.id;
  if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) {
    std::printf("  monitor name: \"%ls\" (%zu characters)\n", target.monitorFriendlyDeviceName, std::wcslen(target.monitorFriendlyDeviceName));
  }

  DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color = {};
  color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
  color.header.size = sizeof(color);
  color.header.adapterId = active.path.targetInfo.adapterId;
  color.header.id = active.path.targetInfo.id;
  if (DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS) {
    std::printf("  advanced color: supported %u, enabled %u, wide color enforced %u, %u bits per channel, encoding %d\n", color.advancedColorSupported, color.advancedColorEnabled, color.wideColorEnforced, color.bitsPerColorChannel, (int) color.colorEncoding);
  }

  DISPLAYCONFIG_SDR_WHITE_LEVEL white = {};
  white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
  white.header.size = sizeof(white);
  white.header.adapterId = active.path.targetInfo.adapterId;
  white.header.id = active.path.targetInfo.id;
  if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS) {
    std::printf("  SDR white level: %.0f nits\n", white.SDRWhiteLevel * 80.0 / 1000.0);
  }

  // What DXGI, and so Apollo and games, see for this output
  IDXGIFactory1 *factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **) &factory))) {
    return;
  }
  IDXGIAdapter1 *adapter = nullptr;
  for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
    DXGI_ADAPTER_DESC1 adapter_desc;
    adapter->GetDesc1(&adapter_desc);
    IDXGIOutput *output = nullptr;
    for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
      DXGI_OUTPUT_DESC desc;
      output->GetDesc(&desc);
      if (active.gdi_name == desc.DeviceName) {
        std::printf("  rendered by: %ls (%llu MB dedicated)\n", adapter_desc.Description, (unsigned long long) adapter_desc.DedicatedVideoMemory / (1024 * 1024));
        IDXGIOutput6 *output6 = nullptr;
        if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), (void **) &output6))) {
          DXGI_OUTPUT_DESC1 d1;
          if (SUCCEEDED(output6->GetDesc1(&d1))) {
            std::printf("  DXGI: color space %d, %u bits per color\n", (int) d1.ColorSpace, d1.BitsPerColor);
            std::printf("  DXGI: luminance min %.4f, max %.1f, max full frame %.1f nits\n", d1.MinLuminance, d1.MaxLuminance, d1.MaxFullFrameLuminance);
            std::printf("  DXGI: red %.4f,%.4f green %.4f,%.4f blue %.4f,%.4f white %.4f,%.4f\n", d1.RedPrimary[0], d1.RedPrimary[1], d1.GreenPrimary[0], d1.GreenPrimary[1], d1.BluePrimary[0], d1.BluePrimary[1], d1.WhitePoint[0], d1.WhitePoint[1]);
          }
          output6->Release();
        }
      }
      output->Release();
    }
    adapter->Release();
  }
  factory->Release();
}

#pragma endregion

#pragma region Install

static std::vector<std::wstring> multi_sz(const std::vector<BYTE> &data) {
  std::vector<std::wstring> out;
  auto *p = (const wchar_t *) data.data();
  size_t n = data.size() / sizeof(wchar_t);
  size_t i = 0;
  while (i < n && p[i]) {
    std::wstring s;
    while (i < n && p[i]) {
      s.push_back(p[i++]);
    }
    out.push_back(s);
    ++i;
  }
  return out;
}

// Removes every device with this hardware ID; returns how many
static int remove_devices(const wchar_t *hardware_id) {
  HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
  if (set == INVALID_HANDLE_VALUE) {
    print_error("Listing devices");
    return 0;
  }

  int removed = 0;
  SP_DEVINFO_DATA dev = {};
  dev.cbSize = sizeof(dev);
  for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
    DWORD size = 0;
    SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, nullptr, 0, &size);
    if (!size) {
      continue;
    }
    std::vector<BYTE> data(size + 2 * sizeof(wchar_t));
    if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, data.data(), size, nullptr)) {
      continue;
    }
    for (const auto &id : multi_sz(data)) {
      if (_wcsicmp(id.c_str(), hardware_id) == 0) {
        // Loaded at run time: MinGW's import library lacks it
        using uninstall_t = BOOL(WINAPI *)(HWND, HDEVINFO, PSP_DEVINFO_DATA, DWORD, PBOOL);
        static auto uninstall = (uninstall_t) (void *) GetProcAddress(LoadLibraryW(L"newdev.dll"), "DiUninstallDevice");
        BOOL reboot = FALSE;
        if (uninstall && uninstall(nullptr, set, &dev, 0, &reboot)) {
          removed++;
        } else {
          print_error("Removing a device");
        }
        break;
      }
    }
  }

  SetupDiDestroyDeviceInfoList(set);
  return removed;
}

static bool device_exists(const wchar_t *hardware_id) {
  HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
  if (set == INVALID_HANDLE_VALUE) {
    return false;
  }

  bool found = false;
  SP_DEVINFO_DATA dev = {};
  dev.cbSize = sizeof(dev);
  for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
    DWORD size = 0;
    SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, nullptr, 0, &size);
    if (!size) {
      continue;
    }
    std::vector<BYTE> data(size + 2 * sizeof(wchar_t));
    if (SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, data.data(), size, nullptr)) {
      for (const auto &id : multi_sz(data)) {
        found = found || _wcsicmp(id.c_str(), hardware_id) == 0;
      }
    }
  }

  SetupDiDestroyDeviceInfoList(set);
  return found;
}

// Deletes the driver packages in the driver store whose catalog has this name; returns how many
static int delete_packages(const wchar_t *catalog) {
  wchar_t windows[MAX_PATH];
  GetWindowsDirectoryW(windows, MAX_PATH);
  std::wstring pattern = std::wstring(windows) + L"\\INF\\oem*.inf";

  int deleted = 0;
  WIN32_FIND_DATAW found;
  HANDLE search = FindFirstFileW(pattern.c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) {
    return 0;
  }
  do {
    std::wstring path = std::wstring(windows) + L"\\INF\\" + found.cFileName;
    HINF inf = SetupOpenInfFileW(path.c_str(), nullptr, INF_STYLE_WIN4, nullptr);
    if (inf == INVALID_HANDLE_VALUE) {
      continue;
    }
    INFCONTEXT line;
    wchar_t value[MAX_PATH] = {};
    bool match = SetupFindFirstLineW(inf, L"Version", L"CatalogFile", &line) && SetupGetStringFieldW(&line, 1, value, MAX_PATH, nullptr) && _wcsicmp(value, catalog) == 0;
    SetupCloseInfFile(inf);

    if (match) {
      if (SetupUninstallOEMInfW(found.cFileName, SUOI_FORCEDELETE, nullptr)) {
        std::printf("Deleted driver package %ls\n", found.cFileName);
        deleted++;
      } else {
        print_error("Deleting a driver package");
      }
    }
  } while (FindNextFileW(search, &found));
  FindClose(search);
  return deleted;
}

static int cmd_install(const char *inf_arg) {
  std::wstring inf_w(inf_arg, inf_arg + std::strlen(inf_arg));
  wchar_t inf[MAX_PATH];
  if (!GetFullPathNameW(inf_w.c_str(), MAX_PATH, inf, nullptr)) {
    print_error("Finding the INF");
    return 1;
  }

  GUID class_guid;
  wchar_t class_name[MAX_CLASS_NAME_LEN];
  if (!SetupDiGetINFClassW(inf, &class_guid, class_name, MAX_CLASS_NAME_LEN, nullptr)) {
    print_error("Reading the INF's class");
    return 1;
  }

  // The root device the driver attaches to, as devcon install creates it
  if (!device_exists(s_HardwareId)) {
    HDEVINFO set = SetupDiCreateDeviceInfoList(&class_guid, nullptr);
    if (set == INVALID_HANDLE_VALUE) {
      print_error("Creating a device list");
      return 1;
    }
    SP_DEVINFO_DATA dev = {};
  dev.cbSize = sizeof(dev);
    static const wchar_t hardware_ids[] = L"Root\\LuzVDA\0";
    bool ok = SetupDiCreateDeviceInfoW(set, class_name, &class_guid, nullptr, nullptr, DICD_GENERATE_ID, &dev) &&
              SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, (const BYTE *) hardware_ids, sizeof(hardware_ids)) &&
              SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &dev);
    if (!ok) {
      print_error("Creating the LuzVDA device");
    }
    SetupDiDestroyDeviceInfoList(set);
    if (!ok) {
      return 1;
    }
    std::printf("Created the LuzVDA device\n");
  }

  BOOL reboot = FALSE;
  if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, s_HardwareId, inf, INSTALLFLAG_FORCE, &reboot)) {
    print_error("Installing the driver (signed with a certificate this PC trusts?)");
    return 1;
  }
  std::printf("Installed the LuzVDA driver%s\n", reboot ? "; restart Windows to finish" : "");
  return 0;
}

static int cmd_uninstall() {
  int devices = remove_devices(s_HardwareId);
  int packages = delete_packages(L"luzvda.cat");
  std::printf("Removed %d LuzVDA device(s) and %d driver package(s)\n", devices, packages);
  return 0;
}

static int cmd_remove_legacy() {
  int devices = remove_devices(s_LegacyHardwareId);
  int packages = delete_packages(L"sudovda.cat");
  std::printf("Removed %d SudoVDA device(s) and %d driver package(s)\n", devices, packages);
  return 0;
}

#pragma endregion

#pragma region Commands

static int cmd_version() {
  device_t device;
  if (!device.ok()) {
    return 1;
  }
  LUZVDA_VERSION v = {};
  if (!request(device.handle, IOCTL_LUZVDA_GET_VERSION, nullptr, 0, &v, sizeof(v), "Reading the version")) {
    return 1;
  }
  LUZVDA_WATCHDOG w = {};
  request(device.handle, IOCTL_LUZVDA_GET_WATCHDOG, nullptr, 0, &w, sizeof(w), "Reading the watchdog");
  std::printf("LuzVDA %u.%u.%u.%u, protocol %u.%u; this tool speaks %u.%u\n", v.driver[0], v.driver[1], v.driver[2], v.driver[3], v.major, v.minor, LUZVDA_PROTOCOL_MAJOR, LUZVDA_PROTOCOL_MINOR);
  std::printf("Watchdog: %u s (%u left)\n", w.timeout_s, w.countdown_s);
  return 0;
}

static int cmd_list(bool details) {
  device_t device;
  if (!device.ok()) {
    return 1;
  }
  LUZVDA_LIST list;
  if (!list_displays(device.handle, list)) {
    return 1;
  }
  std::printf("%u display(s)\n", list.count);
  for (uint32_t i = 0; i < list.count; ++i) {
    if (details) {
      print_display(list.displays[i]);
    } else {
      const auto &e = list.displays[i];
      std::printf("%s  target %u  preferred %ux%u at %.3f Hz\n", guid_string(e.monitor_guid).c_str(), e.target_id, e.preferred_mode.width, e.preferred_mode.height, e.preferred_mode.refresh_mhz / 1000.0);
    }
  }
  return 0;
}

static void usage() {
  std::printf(
    "luzvda-ctl, for LuzVDA protocol %u.%u. Run elevated.\n"
    "\n"
    "  version                      driver version and watchdog\n"
    "  list                         the driver's displays\n"
    "  diag                         each display as Windows and DXGI see it\n"
    "  add <guid> [options]         add a display and keep it until Enter is pressed\n"
    "      --mode WxH@Hz            repeat for more modes; the first is preferred\n"
    "      --preferred N            make mode N (from 0) preferred instead\n"
    "      --name TEXT              the monitor's name\n"
    "      --size WxH               physical size in millimeters\n"
    "      --p3                     Display P3 primaries (default sRGB)\n"
    "      --hdr PEAK,FULL,MIN      HDR with these luminances in nits\n"
    "      --no-hold                return at once (the watchdog removes it within seconds)\n"
    "  remove <guid>\n"
    "  update-modes <guid> --mode WxH@Hz ... [--preferred N]\n"
    "  set-mode <guid> WxH@Hz       ask Windows to switch the display to this mode\n"
    "  hdr <guid> on|off            switch Windows HDR on the display\n"
    "  set-adapter auto|TEXT        render on the GPU with most memory, or whose name contains TEXT\n"
    "  ping\n"
    "  capture-test <guid> [options]  compare the frame channel with Desktop Duplication on the same frames\n"
    "      --add                    add a MacBook Pro-like display with this GUID first, and remove it after\n"
    "      --seconds N              how long to measure (default 20)\n"
    "      --hdr on|off             switch the display's HDR first\n"
    "      --hdr-switch-every N     switch the display's HDR off and on every N seconds\n"
    "      --only channel|duplication\n"
    "      --debug-privilege        enable SeDebugPrivilege, so the channel's handles can be duplicated\n"
    "  install <LuzVDA.inf>         create the device and install the driver\n"
    "  uninstall                    remove the device and its driver packages\n"
    "  remove-legacy                remove SudoVDA's device and driver packages\n",
    LUZVDA_PROTOCOL_MAJOR, LUZVDA_PROTOCOL_MINOR);
}

struct mode_args_t {
  std::vector<LUZVDA_MODE> modes;
  uint32_t preferred = 0;
};

static bool parse_mode_option(int &i, int argc, char **argv, mode_args_t &out) {
  if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
    LUZVDA_MODE m;
    if (!parse_mode(argv[++i], m)) {
      std::fprintf(stderr, "Bad mode %s, expected WxH@Hz\n", argv[i]);
      return false;
    }
    out.modes.push_back(m);
    return true;
  }
  if (std::strcmp(argv[i], "--preferred") == 0 && i + 1 < argc) {
    out.preferred = (uint32_t) std::strtoul(argv[++i], nullptr, 10);
    return true;
  }
  return false;
}

static int cmd_add(int argc, char **argv) {
  LUZVDA_DISPLAY_DESC desc;
  std::memset(&desc, 0, sizeof(desc));
  desc.struct_size = sizeof(desc);
  if (argc < 3 || !parse_guid(argv[2], desc.monitor_guid)) {
    std::fprintf(stderr, "add needs a GUID\n");
    return 2;
  }

  mode_args_t modes;
  bool hold = true;
  for (int i = 3; i < argc; ++i) {
    if (parse_mode_option(i, argc, argv, modes)) {
      continue;
    } else if (std::strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
      std::snprintf(desc.name, sizeof(desc.name), "%s", argv[++i]);
    } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
      unsigned w = 0, h = 0;
      std::sscanf(argv[++i], "%ux%u", &w, &h);
      desc.width_mm = (uint16_t) w;
      desc.height_mm = (uint16_t) h;
    } else if (std::strcmp(argv[i], "--p3") == 0) {
      const uint16_t p3[8] = {696, 328, 271, 707, 154, 61, 320, 337};
      std::memcpy(desc.primaries, p3, sizeof(p3));
    } else if (std::strcmp(argv[i], "--hdr") == 0 && i + 1 < argc) {
      double peak = 0, full = 0, min = 0;
      std::sscanf(argv[++i], "%lf,%lf,%lf", &peak, &full, &min);
      desc.flags |= LUZVDA_DISPLAY_HDR;
      desc.max_luminance = (uint32_t) peak;
      desc.max_full_frame_luminance = (uint32_t) full;
      desc.min_luminance = (uint32_t) std::lround(min * 10000);
    } else if (std::strcmp(argv[i], "--no-hold") == 0) {
      hold = false;
    } else {
      std::fprintf(stderr, "Unknown option %s\n", argv[i]);
      return 2;
    }
  }

  if (modes.modes.empty()) {
    modes.modes.push_back({1920, 1080, 60000});
  }
  if (modes.modes.size() > LUZVDA_MAX_MODES || modes.preferred >= modes.modes.size()) {
    std::fprintf(stderr, "1 to %u modes, and --preferred one of them\n", LUZVDA_MAX_MODES);
    return 2;
  }
  desc.mode_count = (uint32_t) modes.modes.size();
  desc.preferred_mode = modes.preferred;
  std::memcpy(desc.modes, modes.modes.data(), modes.modes.size() * sizeof(LUZVDA_MODE));

  device_t device;
  if (!device.ok()) {
    return 1;
  }

  LUZVDA_ADD_OUT out = {};
  if (!request(device.handle, IOCTL_LUZVDA_ADD_DISPLAY, &desc, sizeof(desc), &out, sizeof(out), "Adding the display")) {
    return 1;
  }
  std::printf("Added: adapter %08lx:%08lx, target %u\n", (unsigned long) out.adapter.high, (unsigned long) out.adapter.low, out.target_id);

  if (!hold) {
    return 0;
  }

  // The watchdog removes displays when nothing asks for a few seconds, so keep pinging
  std::printf("Keeping it until Enter is pressed. Run luzvda-ctl diag in another prompt to inspect it.\n");
  while (!(_kbhit() && _getch() == '\r')) {
    request(device.handle, IOCTL_LUZVDA_PING, nullptr, 0, nullptr, 0, "Pinging");
    Sleep(500);
  }

  request(device.handle, IOCTL_LUZVDA_REMOVE_DISPLAY, &desc.monitor_guid, sizeof(desc.monitor_guid), nullptr, 0, "Removing the display");
  std::printf("Removed\n");
  return 0;
}

static int cmd_remove(int argc, char **argv) {
  LUZVDA_GUID guid;
  if (argc < 3 || !parse_guid(argv[2], guid)) {
    std::fprintf(stderr, "remove needs a GUID\n");
    return 2;
  }
  device_t device;
  if (!device.ok()) {
    return 1;
  }
  return request(device.handle, IOCTL_LUZVDA_REMOVE_DISPLAY, &guid, sizeof(guid), nullptr, 0, "Removing the display") ? 0 : 1;
}

static int cmd_update_modes(int argc, char **argv) {
  LUZVDA_UPDATE_MODES update;
  std::memset(&update, 0, sizeof(update));
  update.struct_size = sizeof(update);
  if (argc < 3 || !parse_guid(argv[2], update.monitor_guid)) {
    std::fprintf(stderr, "update-modes needs a GUID\n");
    return 2;
  }
  mode_args_t modes;
  for (int i = 3; i < argc; ++i) {
    if (!parse_mode_option(i, argc, argv, modes)) {
      std::fprintf(stderr, "Unknown option %s\n", argv[i]);
      return 2;
    }
  }
  if (modes.modes.empty() || modes.modes.size() > LUZVDA_MAX_MODES || modes.preferred >= modes.modes.size()) {
    std::fprintf(stderr, "1 to %u modes, and --preferred one of them\n", LUZVDA_MAX_MODES);
    return 2;
  }
  update.mode_count = (uint32_t) modes.modes.size();
  update.preferred_mode = modes.preferred;
  std::memcpy(update.modes, modes.modes.data(), modes.modes.size() * sizeof(LUZVDA_MODE));

  device_t device;
  if (!device.ok()) {
    return 1;
  }
  if (!request(device.handle, IOCTL_LUZVDA_UPDATE_MODES, &update, sizeof(update), nullptr, 0, "Updating modes")) {
    return 1;
  }
  std::printf("Updated: %u mode(s)\n", update.mode_count);
  return 0;
}

static int cmd_set_mode(int argc, char **argv) {
  LUZVDA_GUID guid;
  LUZVDA_MODE mode;
  if (argc < 4 || !parse_guid(argv[2], guid) || !parse_mode(argv[3], mode)) {
    std::fprintf(stderr, "set-mode needs a GUID and WxH@Hz\n");
    return 2;
  }
  device_t device;
  if (!device.ok()) {
    return 1;
  }
  LUZVDA_LIST_ENTRY entry;
  if (!find_entry(device.handle, guid, entry)) {
    return 1;
  }

  active_display_t active;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  size_t index = 0;
  if (!find_active(entry, active, &paths, &modes, &index)) {
    std::fprintf(stderr, "The display isn't active\n");
    return 1;
  }

  // The same call Apollo makes: the source size and the target's exact rate
  auto &path = paths[index];
  for (auto &m : modes) {
    if (m.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE && m.id == path.sourceInfo.id && m.adapterId.LowPart == path.sourceInfo.adapterId.LowPart && m.adapterId.HighPart == path.sourceInfo.adapterId.HighPart) {
      m.sourceMode.width = mode.width;
      m.sourceMode.height = mode.height;
    }
  }
  path.targetInfo.refreshRate = {mode.refresh_mhz, 1000};

  LONG status = SetDisplayConfig((UINT32) paths.size(), paths.data(), (UINT32) modes.size(), modes.data(), SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE);
  std::printf("SetDisplayConfig: %ld%s\n", status, status == ERROR_SUCCESS ? " (success)" : "");

  if (find_active(entry, active)) {
    std::printf("Now %ux%u at %.3f Hz\n", active.source.width, active.source.height, rate_hz(active.path.targetInfo.refreshRate));
  }
  return status == ERROR_SUCCESS ? 0 : 1;
}

static int cmd_hdr(int argc, char **argv) {
  LUZVDA_GUID guid;
  if (argc < 4 || !parse_guid(argv[2], guid) || (std::strcmp(argv[3], "on") != 0 && std::strcmp(argv[3], "off") != 0)) {
    std::fprintf(stderr, "hdr needs a GUID and on or off\n");
    return 2;
  }
  device_t device;
  if (!device.ok()) {
    return 1;
  }
  LUZVDA_LIST_ENTRY entry;
  active_display_t active;
  if (!find_entry(device.handle, guid, entry) || !find_active(entry, active)) {
    std::fprintf(stderr, "The display isn't active\n");
    return 1;
  }

  DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE state = {};
  state.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
  state.header.size = sizeof(state);
  state.header.adapterId = active.path.targetInfo.adapterId;
  state.header.id = active.path.targetInfo.id;
  state.enableAdvancedColor = std::strcmp(argv[3], "on") == 0;
  LONG status = DisplayConfigSetDeviceInfo(&state.header);
  std::printf("HDR %s: %ld%s\n", argv[3], status, status == ERROR_SUCCESS ? " (success)" : "");
  return status == ERROR_SUCCESS ? 0 : 1;
}

static int cmd_set_adapter(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr, "set-adapter needs auto or part of a GPU's name\n");
    return 2;
  }
  std::wstring wanted(argv[2], argv[2] + std::strlen(argv[2]));
  for (auto &c : wanted) {
    c = towlower(c);
  }
  bool automatic = wanted == L"auto";

  IDXGIFactory1 *factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **) &factory))) {
    print_error("Creating a DXGI factory");
    return 1;
  }

  bool found = false;
  DXGI_ADAPTER_DESC1 best = {};
  IDXGIAdapter1 *adapter = nullptr;
  for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
    DXGI_ADAPTER_DESC1 d;
    adapter->GetDesc1(&d);
    adapter->Release();
    if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || d.VendorId == 0x1414) {
      continue;
    }
    std::wstring name = d.Description;
    for (auto &c : name) {
      c = towlower(c);
    }
    bool match = automatic ? (!found || d.DedicatedVideoMemory > best.DedicatedVideoMemory) : (!found && name.find(wanted) != std::wstring::npos);
    if (match) {
      best = d;
      found = true;
    }
  }
  factory->Release();

  if (!found) {
    std::fprintf(stderr, "No GPU matches\n");
    return 1;
  }

  device_t device;
  if (!device.ok()) {
    return 1;
  }
  LUZVDA_LUID luid = {best.AdapterLuid.LowPart, best.AdapterLuid.HighPart};
  if (!request(device.handle, IOCTL_LUZVDA_SET_RENDER_ADAPTER, &luid, sizeof(luid), nullptr, 0, "Setting the render adapter")) {
    return 1;
  }
  std::printf("Rendering on %ls\n", best.Description);
  return 0;
}

static int cmd_ping() {
  device_t device;
  if (!device.ok()) {
    return 1;
  }
  return request(device.handle, IOCTL_LUZVDA_PING, nullptr, 0, nullptr, 0, "Pinging") ? 0 : 1;
}

#pragma endregion

#pragma region Capture test

// Compares the two ways to get a LuzVDA display's frames: the driver's frame channel (protocol 1.1)
// and Windows' Desktop Duplication, which Apollo uses today. A window on the display draws, in each
// frame, the time just before it was presented (frame_code.h). Both paths take the same frames and
// read that time back, so each path's latency is measured from the same start, frame by frame.

static int64_t qpc_now() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return now.QuadPart;
}

static int64_t qpc_frequency() {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  return f.QuadPart;
}

template <class T>
static void release(T *&p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

struct capture_sample_t {
  uint64_t code;  // QueryPerformanceCounter just before the window presented the frame
  int64_t available;  // when the path had the frame: the channel's ready event, or AcquireNextFrame
  int64_t read;  // once its pixels were on the CPU
  int64_t driver_acquired;  // channel: when Windows gave the driver the frame
  int64_t driver_copied;  // channel: when the driver had queued its copy
  int64_t presented;  // Desktop Duplication: the frame's LastPresentTime
};

struct capture_path_t {
  const char *name;
  bool enabled = true;
  bool failed = false;
  std::string setup;
  std::vector<capture_sample_t> samples;
  uint64_t frames = 0;  // frames taken
  uint64_t undecoded = 0;  // frames whose code didn't read (a tear, or the window not drawn yet)
  std::vector<int64_t> restarts;  // when the path had to start over
  std::string counters;
};

// A texture that holds the code's area of a frame, copied for the CPU to read
struct code_reader_t {
  ID3D11Texture2D *staging = nullptr;
  uint32_t format = 0;

  ~code_reader_t() {
    release(staging);
  }

  // Queues the copy of the code's area; false if the frame is too small or of an unknown format
  bool copy(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *frame) {
    D3D11_TEXTURE2D_DESC desc;
    frame->GetDesc(&desc);
    namespace fc = luzvda::frame_code;
    if ((int) desc.Width < fc::k_left + fc::k_width || (int) desc.Height < fc::k_top + fc::k_height || fc::bytes_per_pixel(desc.Format) == 0) {
      return false;
    }
    if (!staging || format != (uint32_t) desc.Format) {
      release(staging);
      D3D11_TEXTURE2D_DESC s = {};
      s.Width = fc::k_width;
      s.Height = fc::k_height;
      s.MipLevels = 1;
      s.ArraySize = 1;
      s.Format = desc.Format;
      s.SampleDesc.Count = 1;
      s.Usage = D3D11_USAGE_STAGING;
      s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (FAILED(device->CreateTexture2D(&s, nullptr, &staging))) {
        return false;
      }
      format = (uint32_t) desc.Format;
    }
    D3D11_BOX box = {(UINT) fc::k_left, (UINT) fc::k_top, 0, (UINT) (fc::k_left + fc::k_width), (UINT) (fc::k_top + fc::k_height), 1};
    context->CopySubresourceRegion(staging, 0, 0, 0, 0, frame, 0, &box);
    return true;
  }

  // Waits for the copy and decodes it
  bool read(ID3D11DeviceContext *context, uint64_t &code) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
      return false;
    }
    namespace fc = luzvda::frame_code;
    const uint8_t *base = (const uint8_t *) mapped.pData;
    int bpp = fc::bytes_per_pixel(format);
    uint32_t f = format;
    bool ok = fc::decode([&](int x, int y) {
      return fc::is_light(f, base + (size_t) (y - fc::k_top) * mapped.RowPitch + (size_t) (x - fc::k_left) * bpp);
    }, code);
    context->Unmap(staging, 0);
    return ok;
  }
};

static bool make_device(IDXGIAdapter1 *adapter, ID3D11Device1 *&device, ID3D11DeviceContext *&context) {
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  ID3D11Device *base = nullptr;
  HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &base, nullptr, &context);
  if (FAILED(hr)) {
    return false;
  }
  hr = base->QueryInterface(__uuidof(ID3D11Device1), (void **) &device);
  base->Release();
  if (FAILED(hr)) {
    release(context);
    return false;
  }
  return true;
}

static IDXGIAdapter1 *adapter_by_luid(const LUID &luid) {
  IDXGIFactory4 *factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void **) &factory))) {
    return nullptr;
  }
  IDXGIAdapter1 *adapter = nullptr;
  factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), (void **) &adapter);
  factory->Release();
  return adapter;
}

static std::string hex(HRESULT hr) {
  char out[16];
  std::snprintf(out, sizeof(out), "0x%08lX", (unsigned long) hr);
  return out;
}

static void run_channel(HANDLE driver, const LUZVDA_GUID &guid, capture_path_t &path, const std::atomic<bool> &stop) {
  luzvda::frames::consumer_t channel;
  DWORD error = channel.open(driver, guid);
  if (error) {
    path.failed = true;
    path.setup = "opening the channel failed, error " + std::to_string(error) + (error == ERROR_INVALID_FUNCTION ? " (a driver older than protocol 1.1?)" : "");
    return;
  }

  // The textures, and the GPU they're on, come with the first frame the driver copies
  for (int i = 0; i < 40 && !channel.has_textures() && !stop; ++i) {
    channel.heartbeat();
    Sleep(50);
  }
  if (!channel.has_textures()) {
    path.failed = true;
    path.setup = "no frame within 2 s; the driver's last error " + hex((HRESULT) channel.header()->last_error);
    return;
  }

  IDXGIAdapter1 *adapter = adapter_by_luid(channel.adapter());
  ID3D11Device1 *device = nullptr;
  ID3D11DeviceContext *context = nullptr;
  bool made = adapter && make_device(adapter, device, context);
  release(adapter);
  if (!made) {
    path.failed = true;
    path.setup = "no Direct3D device on the textures' GPU";
    return;
  }
  channel.use_device(device);

  code_reader_t reader;
  uint32_t generation = 0;
  while (!stop) {
    luzvda::frames::consumer_t::frame_t frame;
    auto result = channel.next(250, frame);
    if (result == luzvda::frames::consumer_t::result_e::timeout) {
      continue;
    }
    if (result == luzvda::frames::consumer_t::result_e::closed) {
      path.setup += "; the display was removed";
      break;
    }
    if (result == luzvda::frames::consumer_t::result_e::failed) {
      path.failed = true;
      path.setup += "; taking a frame failed, " + hex(channel.error());
      break;
    }

    int64_t available = frame.ready_qpc ? frame.ready_qpc : qpc_now();
    if (frame.generation != generation) {
      if (generation) {
        path.restarts.push_back(available);
      }
      generation = frame.generation;
    }
    bool copied = reader.copy(device, context, frame.texture);
    channel.release(frame);
    path.frames++;

    uint64_t code = 0;
    bool ok = copied && reader.read(context, code);
    int64_t read = qpc_now();
    if (!ok) {
      path.undecoded++;
      continue;
    }
    path.samples.push_back({code, available, read, frame.acquired_qpc, frame.copied_qpc, 0});
  }

  const LUZVDA_FRAME_HEADER *h = channel.header();
  char text[256];
  std::snprintf(text, sizeof(text), "the driver copied %llu, dropped %llu (no free slot); generation %u, %ux%u, DXGI format %u; last error %s",
    (unsigned long long) h->frames_copied, (unsigned long long) h->frames_dropped, h->generation, h->width, h->height, h->dxgi_format, hex((HRESULT) h->last_error).c_str());
  path.counters = text;
  path.setup = std::string("header and event opened by ") + channel.header_opened_by() + ", textures by " + channel.textures_opened_by() + path.setup;

  channel.close();
  release(context);
  release(device);
}

// The output whose GDI name is gdi_name, and its adapter
static bool find_output(const std::wstring &gdi_name, IDXGIAdapter1 *&adapter_out, IDXGIOutput1 *&output_out) {
  IDXGIFactory1 *factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **) &factory))) {
    return false;
  }
  IDXGIAdapter1 *adapter = nullptr;
  for (UINT a = 0; !output_out && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
    IDXGIOutput *output = nullptr;
    for (UINT o = 0; !output_out && adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
      DXGI_OUTPUT_DESC desc;
      if (SUCCEEDED(output->GetDesc(&desc)) && gdi_name == desc.DeviceName && SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1), (void **) &output_out))) {
        adapter_out = adapter;
        adapter->AddRef();
      }
      output->Release();
    }
    adapter->Release();
  }
  factory->Release();
  return output_out != nullptr;
}

static void run_duplication(const std::wstring &gdi_name, capture_path_t &path, const std::atomic<bool> &stop) {
  IDXGIAdapter1 *adapter = nullptr;
  IDXGIOutput1 *output = nullptr;
  if (!find_output(gdi_name, adapter, output)) {
    path.failed = true;
    path.setup = "no DXGI output for the display";
    return;
  }
  ID3D11Device1 *device = nullptr;
  ID3D11DeviceContext *context = nullptr;
  bool made = make_device(adapter, device, context);
  release(adapter);
  if (!made) {
    release(output);
    path.failed = true;
    path.setup = "no Direct3D device on the display's GPU";
    return;
  }

  // As Apollo does: FP16 when the display is in HDR, 8-bit otherwise
  IDXGIOutput5 *output5 = nullptr;
  output->QueryInterface(__uuidof(IDXGIOutput5), (void **) &output5);
  IDXGIOutputDuplication *duplication = nullptr;
  auto duplicate = [&]() {
    if (output5) {
      const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
      return output5->DuplicateOutput1(device, 0, 2, formats, &duplication);
    }
    return output->DuplicateOutput(device, &duplication);
  };
  HRESULT hr = duplicate();
  if (FAILED(hr)) {
    path.failed = true;
    path.setup = (output5 ? "DuplicateOutput1 failed, " : "DuplicateOutput failed, ") + hex(hr) + (hr == E_ACCESSDENIED ? " (the desktop is locked or on the secure desktop?)" : "");
  } else {
    path.setup = output5 ? "DuplicateOutput1 (FP16 or 8-bit)" : "DuplicateOutput (8-bit)";
  }

  code_reader_t reader;
  while (!stop && duplication) {
    DXGI_OUTDUPL_FRAME_INFO info;
    IDXGIResource *resource = nullptr;
    hr = duplication->AcquireNextFrame(250, &info, &resource);
    int64_t available = qpc_now();
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
      continue;
    }
    if (FAILED(hr)) {
      // ACCESS_LOST on a mode or HDR change, a full-screen program or the secure desktop: start over
      path.restarts.push_back(available);
      release(duplication);
      while (!stop && FAILED(duplicate())) {
        Sleep(20);
      }
      continue;
    }

    if (info.LastPresentTime.QuadPart == 0) {
      // Only the pointer changed
      release(resource);
      duplication->ReleaseFrame();
      continue;
    }

    ID3D11Texture2D *frame = nullptr;
    bool copied = SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void **) &frame)) && reader.copy(device, context, frame);
    release(frame);
    release(resource);
    duplication->ReleaseFrame();
    path.frames++;

    uint64_t code = 0;
    bool ok = copied && reader.read(context, code);
    int64_t read = qpc_now();
    if (!ok) {
      path.undecoded++;
      continue;
    }
    path.samples.push_back({code, available, read, 0, 0, info.LastPresentTime.QuadPart});
  }

  release(duplication);
  release(output5);
  release(output);
  release(context);
  release(device);
}

// A small window on the display that draws the code, one new value per frame
struct pattern_window_t {
  HWND hwnd = nullptr;
  ID3D11Device1 *device = nullptr;
  ID3D11DeviceContext *context = nullptr;
  ID3D11DeviceContext1 *context1 = nullptr;
  IDXGISwapChain1 *chain = nullptr;
  ID3D11RenderTargetView *target = nullptr;
  // The window's client area starts here, in the display's pixels
  int origin_x = luzvda::frame_code::k_left - 16;
  int origin_y = luzvda::frame_code::k_top - 16;

  ~pattern_window_t() {
    release(target);
    release(chain);
    release(context1);
    release(context);
    release(device);
    if (hwnd) {
      DestroyWindow(hwnd);
    }
  }

  std::string create(const RECT &monitor) {
    namespace fc = luzvda::frame_code;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LuzVDACaptureTest";
    RegisterClassW(&wc);
    int width = fc::k_width + 32;
    int height = fc::k_height + 32;
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"LuzVDA capture test", WS_POPUP, monitor.left + origin_x, monitor.top + origin_y, width, height, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
      return "creating the window failed, error " + std::to_string(GetLastError());
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    if (!make_device(nullptr, device, context) || FAILED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **) &context1))) {
      return "no Direct3D 11.1 device to draw with";
    }
    D3D11_FEATURE_DATA_D3D11_OPTIONS options = {};
    device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options));
    if (!options.ClearView) {
      return "the GPU can't ClearView, which drawing the code needs";
    }

    IDXGIDevice *dxgi_device = nullptr;
    IDXGIAdapter *adapter = nullptr;
    IDXGIFactory2 *factory = nullptr;
    HRESULT hr = device->QueryInterface(__uuidof(IDXGIDevice), (void **) &dxgi_device);
    if (SUCCEEDED(hr)) {
      hr = dxgi_device->GetAdapter(&adapter);
    }
    if (SUCCEEDED(hr)) {
      hr = adapter->GetParent(__uuidof(IDXGIFactory2), (void **) &factory);
    }
    if (SUCCEEDED(hr)) {
      DXGI_SWAP_CHAIN_DESC1 desc = {};
      desc.Width = (UINT) width;
      desc.Height = (UINT) height;
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      desc.BufferCount = 2;
      desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
      hr = factory->CreateSwapChainForHwnd(device, hwnd, &desc, nullptr, nullptr, &chain);
    }
    release(factory);
    release(adapter);
    release(dxgi_device);
    ID3D11Texture2D *buffer = nullptr;
    if (SUCCEEDED(hr)) {
      hr = chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **) &buffer);
    }
    if (SUCCEEDED(hr)) {
      hr = device->CreateRenderTargetView(buffer, nullptr, &target);
    }
    release(buffer);
    return SUCCEEDED(hr) ? "" : "making the window's swap chain failed, " + hex(hr);
  }

  // Draws the current time as the code and presents it at the next refresh; returns the code
  uint64_t draw() {
    const float black[4] = {0, 0, 0, 1};
    const float white[4] = {1, 1, 1, 1};
    context->OMSetRenderTargets(1, &target, nullptr);
    context->ClearRenderTargetView(target, black);
    uint64_t code = (uint64_t) qpc_now();
    std::vector<D3D11_RECT> rects;
    for (const auto &r : luzvda::frame_code::white_blocks(code)) {
      rects.push_back({r.left - origin_x, r.top - origin_y, r.right - origin_x, r.bottom - origin_y});
    }
    context1->ClearView(target, white, rects.data(), (UINT) rects.size());
    chain->Present(1, 0);
    return code;
  }
};

static bool monitor_rect(const std::wstring &gdi_name, RECT &out) {
  struct search_t {
    const std::wstring *name;
    RECT rect;
    bool found;
  } search = {&gdi_name, {}, false};
  EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
    auto *s = (search_t *) data;
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) && *s->name == info.szDevice) {
      s->rect = info.rcMonitor;
      s->found = true;
      return FALSE;
    }
    return TRUE;
  }, (LPARAM) &search);
  out = search.rect;
  return search.found;
}

static bool set_hdr(const LUZVDA_LIST_ENTRY &entry, bool on) {
  active_display_t active;
  if (!find_active(entry, active)) {
    return false;
  }
  DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE state = {};
  state.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
  state.header.size = sizeof(state);
  state.header.adapterId = active.path.targetInfo.adapterId;
  state.header.id = active.path.targetInfo.id;
  state.enableAdvancedColor = on;
  return DisplayConfigSetDeviceInfo(&state.header) == ERROR_SUCCESS;
}

static bool enable_privilege(const wchar_t *name) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
    return false;
  }
  TOKEN_PRIVILEGES privileges = {};
  privileges.PrivilegeCount = 1;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  bool ok = LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid) && AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
  CloseHandle(token);
  return ok;
}

static double percentile(std::vector<double> values, double p) {
  if (values.empty()) {
    return 0;
  }
  std::sort(values.begin(), values.end());
  size_t i = (size_t) std::lround(p / 100.0 * (double) (values.size() - 1));
  return values[std::min(i, values.size() - 1)];
}

static std::string spread(const std::vector<double> &values) {
  if (values.empty()) {
    return "none";
  }
  char out[128];
  std::snprintf(out, sizeof(out), "p50 %.2f, p95 %.2f, p99 %.2f, max %.2f ms", percentile(values, 50), percentile(values, 95), percentile(values, 99), percentile(values, 100));
  return out;
}

// The samples worth counting: each code once, from after the warm-up
static std::vector<capture_sample_t> unique_samples(const capture_path_t &path, int64_t from) {
  std::vector<capture_sample_t> out;
  std::unordered_set<uint64_t> seen;
  for (const auto &s : path.samples) {
    if ((int64_t) s.code >= from && seen.insert(s.code).second) {
      out.push_back(s);
    }
  }
  return out;
}

// The longest wait for a new frame in [from, to), in ms
static double longest_gap(const std::vector<capture_sample_t> &samples, int64_t from, int64_t to, double ms_per_tick) {
  int64_t last = from;
  double longest = 0;
  for (const auto &s : samples) {
    if (s.available < from || s.available >= to) {
      continue;
    }
    longest = std::max(longest, (double) (s.available - last) * ms_per_tick);
    last = s.available;
  }
  return std::max(longest, (double) (to - last) * ms_per_tick);
}

static void report_path(const capture_path_t &path, int64_t from, int64_t to, uint64_t presented, const std::vector<int64_t> &toggles, double ms_per_tick) {
  std::printf("\n%s: %s\n", path.name, path.setup.c_str());
  if (!path.enabled) {
    std::printf("  not run\n");
    return;
  }
  auto samples = unique_samples(path, from);
  std::vector<double> available, read, to_driver, driver_copy, to_wake, to_present;
  for (const auto &s : samples) {
    available.push_back((double) (s.available - (int64_t) s.code) * ms_per_tick);
    read.push_back((double) (s.read - (int64_t) s.code) * ms_per_tick);
    if (s.driver_acquired) {
      to_driver.push_back((double) (s.driver_acquired - (int64_t) s.code) * ms_per_tick);
      driver_copy.push_back((double) (s.driver_copied - s.driver_acquired) * ms_per_tick);
      to_wake.push_back((double) (s.available - s.driver_copied) * ms_per_tick);
    }
    if (s.presented) {
      to_present.push_back((double) (s.presented - (int64_t) s.code) * ms_per_tick);
    }
  }
  std::printf("  frames taken %llu, codes read %zu after the warm-up (%.1f%% of the %llu presented), unreadable %llu\n", (unsigned long long) path.frames, samples.size(), presented ? 100.0 * (double) samples.size() / (double) presented : 0.0, (unsigned long long) presented, (unsigned long long) path.undecoded);
  std::printf("  presented -> available: %s\n", spread(available).c_str());
  std::printf("  presented -> pixels on the CPU: %s\n", spread(read).c_str());
  if (!to_driver.empty()) {
    std::printf("  presented -> driver has it: %s\n", spread(to_driver).c_str());
    std::printf("  driver queues the copy: %s\n", spread(driver_copy).c_str());
    std::printf("  copy queued -> consumer wakes: %s\n", spread(to_wake).c_str());
  }
  if (!to_present.empty()) {
    std::printf("  presented -> Windows' LastPresentTime: %s\n", spread(to_present).c_str());
  }
  std::printf("  longest wait for a new frame: %.1f ms\n", longest_gap(samples, from, to, ms_per_tick));
  std::printf("  started over %zu times\n", path.restarts.size());
  for (size_t i = 0; i < toggles.size(); ++i) {
    int64_t end = std::min(to, toggles[i] + (int64_t) (5000.0 / ms_per_tick));
    std::printf("  HDR switch %zu: longest wait in the 5 s after it %.1f ms\n", i + 1, longest_gap(samples, toggles[i], end, ms_per_tick));
  }
  if (!path.counters.empty()) {
    std::printf("  %s\n", path.counters.c_str());
  }
}

static int cmd_capture_test(int argc, char **argv) {
  LUZVDA_GUID guid;
  if (argc < 3 || !parse_guid(argv[2], guid)) {
    std::fprintf(stderr, "capture-test needs a GUID\n");
    return 2;
  }
  double seconds = 20;
  double toggle_every = 0;
  bool add = false;
  bool debug_privilege = false;
  std::string only;
  std::string hdr;
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      seconds = std::atof(argv[++i]);
    } else if (std::strcmp(argv[i], "--hdr-switch-every") == 0 && i + 1 < argc) {
      toggle_every = std::atof(argv[++i]);
    } else if (std::strcmp(argv[i], "--add") == 0) {
      add = true;
    } else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
      only = argv[++i];
    } else if (std::strcmp(argv[i], "--debug-privilege") == 0) {
      debug_privilege = true;
    } else if (std::strcmp(argv[i], "--hdr") == 0 && i + 1 < argc) {
      hdr = argv[++i];
    } else {
      std::fprintf(stderr, "Unknown option %s\n", argv[i]);
      return 2;
    }
  }
  if (seconds < 3 || (!only.empty() && only != "channel" && only != "duplication") || (!hdr.empty() && hdr != "on" && hdr != "off")) {
    std::fprintf(stderr, "--seconds is at least 3, --only is channel or duplication, and --hdr is on or off\n");
    return 2;
  }

  // One window pixel per display pixel, whatever the display's scale
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (debug_privilege) {
    std::printf("SeDebugPrivilege: %s\n", enable_privilege(L"SeDebugPrivilege") ? "enabled" : "not enabled");
  }

  device_t device;
  if (!device.ok()) {
    return 1;
  }
  LUZVDA_VERSION version = {};
  request(device.handle, IOCTL_LUZVDA_GET_VERSION, nullptr, 0, &version, sizeof(version), "Reading the version");
  std::printf("LuzVDA %u.%u.%u.%u, protocol %u.%u\n", version.driver[0], version.driver[1], version.driver[2], version.driver[3], version.major, version.minor);

  if (add) {
    // As Apollo does: render on the GPU with the most memory, then a MacBook Pro-like display
    char *set_adapter[] = {argv[0], (char *) "set-adapter", (char *) "auto"};
    cmd_set_adapter(3, set_adapter);

    LUZVDA_DISPLAY_DESC desc;
    std::memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.monitor_guid = guid;
    std::snprintf(desc.name, sizeof(desc.name), "Capture test");
    desc.width_mm = 302;
    desc.height_mm = 196;
    const uint16_t p3[8] = {696, 328, 271, 707, 154, 61, 320, 337};
    std::memcpy(desc.primaries, p3, sizeof(p3));
    desc.flags = LUZVDA_DISPLAY_HDR;
    desc.max_luminance = 1600;
    desc.max_full_frame_luminance = 1000;
    desc.min_luminance = 50;
    desc.mode_count = 3;
    desc.modes[0] = {3024, 1964, 120000};
    desc.modes[1] = {3024, 1890, 120000};
    desc.modes[2] = {3024, 1964, 60000};
    LUZVDA_ADD_OUT out = {};
    if (!request(device.handle, IOCTL_LUZVDA_ADD_DISPLAY, &desc, sizeof(desc), &out, sizeof(out), "Adding the display")) {
      return 1;
    }
  }

  // Wait for Windows to make the display active
  LUZVDA_LIST_ENTRY entry = {};
  active_display_t active;
  bool found = false;
  for (int i = 0; i < 50 && !found; ++i) {
    request(device.handle, IOCTL_LUZVDA_PING, nullptr, 0, nullptr, 0, "Pinging");
    LUZVDA_LIST list;
    if (list_displays(device.handle, list)) {
      for (uint32_t d = 0; d < list.count; ++d) {
        if (std::memcmp(list.displays[d].monitor_guid.bytes, guid.bytes, 16) == 0) {
          entry = list.displays[d];
          found = find_active(entry, active);
        }
      }
    }
    if (!found) {
      Sleep(100);
    }
  }
  int status = 0;
  RECT monitor = {};
  if (!found || !monitor_rect(active.gdi_name, monitor)) {
    std::fprintf(stderr, "The display isn't active in Windows (add it, or pass --add)\n");
    status = 1;
  }

  if (!status && !hdr.empty()) {
    std::printf("HDR %s before measuring: %s\n", hdr.c_str(), set_hdr(entry, hdr == "on") ? "ok" : "failed");
  }

  pattern_window_t window;
  if (!status) {
    // Let Windows settle after adding the display, or switching HDR
    for (int i = 0; i < 20; ++i) {
      request(device.handle, IOCTL_LUZVDA_PING, nullptr, 0, nullptr, 0, "Pinging");
      Sleep(100);
    }
    monitor_rect(active.gdi_name, monitor);
    find_active(entry, active);
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color = {};
    color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    color.header.size = sizeof(color);
    color.header.adapterId = active.path.targetInfo.adapterId;
    color.header.id = active.path.targetInfo.id;
    bool color_known = DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS;
    std::printf("Display %ls: %ldx%ld at %ld,%ld, %.3f Hz, HDR %s\n", active.gdi_name.c_str(), monitor.right - monitor.left, monitor.bottom - monitor.top, monitor.left, monitor.top, rate_hz(active.path.targetInfo.refreshRate), !color_known ? "unknown" : color.advancedColorEnabled ? "on" : "off");
    std::string error = window.create(monitor);
    if (!error.empty()) {
      std::fprintf(stderr, "The code's window: %s\n", error.c_str());
      status = 1;
    }
  }

  if (!status) {
    capture_path_t channel;
    channel.name = "Frame channel";
    channel.enabled = only.empty() || only == "channel";
    capture_path_t duplication;
    duplication.name = "Desktop Duplication";
    duplication.enabled = only.empty() || only == "duplication";

    double ms_per_tick = 1000.0 / (double) qpc_frequency();
    std::atomic<bool> stop{false};

    // Something on the display first, so the paths have frames from the start
    for (int i = 0; i < 30; ++i) {
      window.draw();
    }
    std::thread channel_thread, duplication_thread;
    if (channel.enabled) {
      channel_thread = std::thread(run_channel, device.handle, std::cref(guid), std::ref(channel), std::cref(stop));
    }
    if (duplication.enabled) {
      duplication_thread = std::thread(run_duplication, std::cref(active.gdi_name), std::ref(duplication), std::cref(stop));
    }

    int64_t start = qpc_now();
    int64_t warm = start + (int64_t) (1000.0 / ms_per_tick);
    int64_t end = start + (int64_t) (seconds * 1000.0 / ms_per_tick);
    int64_t next_ping = start;
    int64_t next_toggle = toggle_every > 0 ? start + (int64_t) (toggle_every * 1000.0 / ms_per_tick) : INT64_MAX;
    bool hdr_on = hdr != "off";
    uint64_t presented = 0;
    std::vector<int64_t> toggles;
    for (int64_t now = start; now < end; now = qpc_now()) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      if ((int64_t) window.draw() >= warm) {
        presented++;
      }
      if (now >= next_ping) {
        request(device.handle, IOCTL_LUZVDA_PING, nullptr, 0, nullptr, 0, "Pinging");
        next_ping = now + (int64_t) (1000.0 / ms_per_tick);
      }
      if (now >= next_toggle) {
        hdr_on = !hdr_on;
        toggles.push_back(qpc_now());
        std::printf("HDR %s: %s\n", hdr_on ? "on" : "off", set_hdr(entry, hdr_on) ? "ok" : "failed");
        next_toggle = now + (int64_t) (toggle_every * 1000.0 / ms_per_tick);
      }
    }
    int64_t finished = qpc_now();
    stop = true;
    if (channel_thread.joinable()) {
      channel_thread.join();
    }
    if (duplication_thread.joinable()) {
      duplication_thread.join();
    }
    if (toggles.size() % 2) {
      set_hdr(entry, hdr != "off");
    }

    std::printf("\nThe window presented %llu frames in %.1f s after a 1 s warm-up (%.1f per second)\n", (unsigned long long) presented, (double) (finished - warm) * ms_per_tick / 1000.0, (double) presented * 1000.0 / ((double) (finished - warm) * ms_per_tick));
    report_path(channel, warm, finished, presented, toggles, ms_per_tick);
    report_path(duplication, warm, finished, presented, toggles, ms_per_tick);

    // The same frames through both: which had each first
    auto a = unique_samples(channel, warm);
    auto b = unique_samples(duplication, warm);
    std::unordered_map<uint64_t, int64_t> by_code;
    for (const auto &s : b) {
      by_code[s.code] = s.available;
    }
    std::vector<double> lead;
    for (const auto &s : a) {
      auto it = by_code.find(s.code);
      if (it != by_code.end()) {
        lead.push_back((double) (s.available - it->second) * ms_per_tick);
      }
    }
    std::printf("\nFrames both paths took: %zu. Frame channel minus Desktop Duplication, when each had the frame (negative: the channel first): %s\n", lead.size(), spread(lead).c_str());

    std::vector<double> ca, da;
    for (const auto &s : a) {
      ca.push_back((double) (s.available - (int64_t) s.code) * ms_per_tick);
    }
    for (const auto &s : b) {
      da.push_back((double) (s.available - (int64_t) s.code) * ms_per_tick);
    }
    std::printf("RESULT channel=%s duplication=%s presented=%llu channel_frames=%zu duplication_frames=%zu channel_p50_ms=%.2f duplication_p50_ms=%.2f channel_p95_ms=%.2f duplication_p95_ms=%.2f channel_restarts=%zu duplication_restarts=%zu\n",
      !channel.enabled ? "skipped" : channel.failed ? "failed" : "ok", !duplication.enabled ? "skipped" : duplication.failed ? "failed" : "ok",
      (unsigned long long) presented, a.size(), b.size(), percentile(ca, 50), percentile(da, 50), percentile(ca, 95), percentile(da, 95), channel.restarts.size(), duplication.restarts.size());
    if ((channel.enabled && channel.failed) || (duplication.enabled && duplication.failed)) {
      status = 1;
    }
  }

  if (add) {
    request(device.handle, IOCTL_LUZVDA_REMOVE_DISPLAY, &guid, sizeof(guid), nullptr, 0, "Removing the display");
  }
  return status;
}

#pragma endregion

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 2;
  }

  CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  std::string cmd = argv[1];
  if (cmd == "version") return cmd_version();
  if (cmd == "list") return cmd_list(false);
  if (cmd == "diag") return cmd_version() || cmd_list(true);
  if (cmd == "add") return cmd_add(argc, argv);
  if (cmd == "remove") return cmd_remove(argc, argv);
  if (cmd == "update-modes") return cmd_update_modes(argc, argv);
  if (cmd == "set-mode") return cmd_set_mode(argc, argv);
  if (cmd == "hdr") return cmd_hdr(argc, argv);
  if (cmd == "set-adapter") return cmd_set_adapter(argc, argv);
  if (cmd == "ping") return cmd_ping();
  if (cmd == "capture-test") return cmd_capture_test(argc, argv);
  if (cmd == "install" && argc >= 3) return cmd_install(argv[2]);
  if (cmd == "uninstall") return cmd_uninstall();
  if (cmd == "remove-legacy") return cmd_remove_legacy();

  usage();
  return 2;
}
