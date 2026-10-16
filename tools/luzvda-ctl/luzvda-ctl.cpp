// luzvda-ctl: installs LuzVDA, removes SudoVDA, and adds, inspects and removes displays by hand.
// Run it from an elevated prompt: the driver opens only for SYSTEM and Administrators.
//
// Builds with MSVC and MinGW:
//   cl /std:c++17 /EHsc /O2 /I..\..\include luzvda-ctl.cpp setupapi.lib newdev.lib dxgi.lib ole32.lib user32.lib
//   x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -I../../include luzvda-ctl.cpp -lsetupapi -lnewdev -ldxgi -lole32 -luser32
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

// MSVC's setupapi.h has it; MinGW's doesn't
#ifndef MAX_CLASS_NAME_LEN
#define MAX_CLASS_NAME_LEN 32
#endif

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
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
      std::printf("%s  target %u  %ux%u at %.3f Hz\n", guid_string(e.monitor_guid).c_str(), e.target_id, e.preferred_mode.width, e.preferred_mode.height, e.preferred_mode.refresh_mhz / 1000.0);
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
  if (cmd == "install" && argc >= 3) return cmd_install(argv[2]);
  if (cmd == "uninstall") return cmd_uninstall();
  if (cmd == "remove-legacy") return cmd_remove_legacy();

  usage();
  return 2;
}
