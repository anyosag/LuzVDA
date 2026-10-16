/*
 * Checks on what callers send the driver, and the mode list built from it. Pure C++17 with no
 * Windows headers, so the tests build it anywhere.
 *
 * SPDX-License-Identifier: MS-PL
 */
#pragma once

#include <luzvda/luzvda-ioctl.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace luzvda {
  enum class check_e {
    ok,
    too_small,       ///< The buffer is smaller than the structure
    bad_struct_size, ///< struct_size isn't this protocol's
    bad_flags,       ///< Unknown flag bits
    bad_name,        ///< No NUL inside the name array
    bad_mode_count,  ///< 0, or more than LUZVDA_MAX_MODES
    bad_preferred,   ///< preferred_mode isn't one of the modes
    bad_mode,        ///< A mode's size, rate or pixel rate is out of range
    bad_size_mm,     ///< Physical size over 2 m
    bad_primaries,   ///< A chromaticity over 1023, or primaries without a white point
    bad_luminance,   ///< HDR without a peak, or values out of order or range
  };

  constexpr uint32_t min_dimension = 320;
  constexpr uint32_t max_dimension = 8192;
  constexpr uint32_t min_refresh_mhz = 1000;
  constexpr uint32_t max_refresh_mhz = 500000;
  /// 7680x4320 at 240 Hz, with room to spare
  constexpr uint64_t max_pixels_per_second = 8000000000ull;
  constexpr uint16_t max_size_mm = 2000;
  constexpr uint32_t max_nits = 10000;

  inline const char *describe(check_e c) {
    switch (c) {
      case check_e::ok: return "ok";
      case check_e::too_small: return "buffer too small";
      case check_e::bad_struct_size: return "struct_size mismatch";
      case check_e::bad_flags: return "unknown flags";
      case check_e::bad_name: return "name not terminated";
      case check_e::bad_mode_count: return "mode count out of range";
      case check_e::bad_preferred: return "preferred mode out of range";
      case check_e::bad_mode: return "mode out of range";
      case check_e::bad_size_mm: return "physical size out of range";
      case check_e::bad_primaries: return "primaries out of range";
      case check_e::bad_luminance: return "luminance out of range";
    }
    return "?";
  }

  inline bool terminated(const char *s, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
      if (s[i] == '\0') {
        return true;
      }
    }
    return false;
  }

  inline bool mode_ok(const LUZVDA_MODE &m) {
    if (m.width < min_dimension || m.width > max_dimension || m.height < min_dimension || m.height > max_dimension) {
      return false;
    }
    if (m.refresh_mhz < min_refresh_mhz || m.refresh_mhz > max_refresh_mhz) {
      return false;
    }
    return (uint64_t) m.width * m.height * m.refresh_mhz / 1000 <= max_pixels_per_second;
  }

  inline check_e check_modes(uint32_t count, uint32_t preferred, const LUZVDA_MODE *modes) {
    if (count == 0 || count > LUZVDA_MAX_MODES) {
      return check_e::bad_mode_count;
    }
    if (preferred >= count) {
      return check_e::bad_preferred;
    }
    for (uint32_t i = 0; i < count; ++i) {
      if (!mode_ok(modes[i])) {
        return check_e::bad_mode;
      }
    }
    return check_e::ok;
  }

  inline check_e check(const LUZVDA_DISPLAY_DESC &d, std::size_t length) {
    if (length < sizeof(LUZVDA_DISPLAY_DESC)) {
      return check_e::too_small;
    }
    if (d.struct_size != sizeof(LUZVDA_DISPLAY_DESC)) {
      return check_e::bad_struct_size;
    }
    if (d.flags & ~LUZVDA_DISPLAY_HDR) {
      return check_e::bad_flags;
    }
    if (!terminated(d.name, sizeof(d.name))) {
      return check_e::bad_name;
    }
    if (auto c = check_modes(d.mode_count, d.preferred_mode, d.modes); c != check_e::ok) {
      return c;
    }
    if (d.width_mm > max_size_mm || d.height_mm > max_size_mm) {
      return check_e::bad_size_mm;
    }

    bool any_primary = false;
    for (auto p : d.primaries) {
      if (p > 1023) {
        return check_e::bad_primaries;
      }
      any_primary = any_primary || p;
    }
    if (any_primary && (d.primaries[6] == 0 || d.primaries[7] == 0)) {
      return check_e::bad_primaries;
    }

    if (d.flags & LUZVDA_DISPLAY_HDR) {
      if (d.max_luminance == 0 || d.max_luminance > max_nits || d.max_full_frame_luminance > d.max_luminance) {
        return check_e::bad_luminance;
      }
      // Black below peak
      if ((uint64_t) d.min_luminance >= (uint64_t) d.max_luminance * 10000) {
        return check_e::bad_luminance;
      }
    }

    return check_e::ok;
  }

  inline check_e check(const LUZVDA_UPDATE_MODES &u, std::size_t length) {
    if (length < sizeof(LUZVDA_UPDATE_MODES)) {
      return check_e::too_small;
    }
    if (u.struct_size != sizeof(LUZVDA_UPDATE_MODES)) {
      return check_e::bad_struct_size;
    }
    return check_modes(u.mode_count, u.preferred_mode, u.modes);
  }

  /// The modes the display offers: the caller's, without repeats, plus 1920x1080 at 60 Hz so Windows
  /// always has a mode it can fall back to
  struct mode_list_t {
    std::vector<LUZVDA_MODE> modes;
    std::size_t preferred = 0;
  };

  inline bool same_mode(const LUZVDA_MODE &a, const LUZVDA_MODE &b) {
    return a.width == b.width && a.height == b.height && a.refresh_mhz == b.refresh_mhz;
  }

  inline mode_list_t mode_list(uint32_t count, uint32_t preferred, const LUZVDA_MODE *modes) {
    mode_list_t list;
    for (uint32_t i = 0; i < count; ++i) {
      bool seen = false;
      for (std::size_t j = 0; j < list.modes.size(); ++j) {
        if (same_mode(list.modes[j], modes[i])) {
          seen = true;
          if (i == preferred) {
            list.preferred = j;
          }
          break;
        }
      }
      if (!seen) {
        if (i == preferred) {
          list.preferred = list.modes.size();
        }
        list.modes.push_back(modes[i]);
      }
    }

    LUZVDA_MODE fallback {1920, 1080, 60000};
    bool has_fallback = false;
    for (const auto &m : list.modes) {
      has_fallback = has_fallback || same_mode(m, fallback);
    }
    if (!has_fallback) {
      list.modes.push_back(fallback);
    }
    return list;
  }

  /// The name as EDID can carry it: printable ASCII only, other bytes dropped
  inline std::string ascii_name(const char *name, std::size_t n) {
    std::string out;
    for (std::size_t i = 0; i < n && name[i]; ++i) {
      auto c = (unsigned char) name[i];
      if (c >= 0x20 && c < 0x7f) {
        out.push_back((char) c);
      }
    }
    return out;
  }
}  // namespace luzvda
