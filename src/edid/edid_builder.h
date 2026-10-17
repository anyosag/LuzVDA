/*
 * The EDID a virtual display hands Windows, built from what the caller says about the screen it
 * mirrors: size, color primaries, HDR luminance, preferred mode and name. Pure C++17 with no Windows
 * headers, so the tests build it anywhere and check it with edid-decode.
 *
 * The layout follows SudoVDA's fixed EDID, which Windows is known to accept: EDID 1.3 with one
 * CTA-861 extension holding the same data blocks. What changed is that the values are generated, and
 * that the parts edid-decode failed are fixed: the primaries were all zero while the sRGB bit was set,
 * the HDMI VSDB claimed a TMDS clock over 340 MHz, a reserved colorimetry bit was set, and there was
 * no Video Capability Data Block.
 *
 * SPDX-License-Identifier: MS-PL
 */
#pragma once

#include <luzvda/luzvda-ioctl.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace luzvda::edid {
  using edid_t = std::array<uint8_t, 256>;

  /// Not in the UEFI PNP ID registry (checked against hwdata's pnp.ids). SudoVDA used SMK, which
  /// belongs to SMK Corporation
  constexpr char manufacturer[] = "LZV";
  constexpr uint16_t product_code = 0x4C56;

  constexpr std::size_t name_chars = 13;

  struct input_t {
    /// Unique per display: serial_for() of its GUID
    uint32_t serial = 1;
    /// Printable ASCII; the first 13 characters are kept
    std::string name;
    /// 0 when unknown
    uint16_t width_mm = 0;
    uint16_t height_mm = 0;
    /// Red, green, blue and white x,y in 1/1024; all 0 means sRGB
    std::array<uint16_t, 8> primaries {};
    bool hdr = false;
    uint32_t max_nits = 0;
    uint32_t max_full_frame_nits = 0;
    /// 1/10000 nits
    uint32_t min_luminance = 0;
    LUZVDA_MODE preferred {1920, 1080, 60000};
    /// An experiment: spread a longer name over two name descriptors, the first 13 characters in place
    /// of the serial text and the next 13 after them, to see whether Windows shows more than 13 (the
    /// driver's edidSplitName setting)
    bool split_name = false;
  };

  /// sRGB / BT.709 primaries with a D65 white point, in 1/1024
  constexpr std::array<uint16_t, 8> srgb_primaries {655, 338, 307, 614, 154, 61, 320, 337};

  /// FNV-1a over the GUID's 16 bytes; never 0, which EDID reads as "no serial"
  inline uint32_t serial_for(const uint8_t guid[16]) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 16; ++i) {
      h ^= guid[i];
      h *= 16777619u;
    }
    return h ? h : 1;
  }

  /// CTA-861.3 maximum and maximum frame-average luminance: 50 * 2^(code / 32) nits
  inline uint8_t luminance_code(uint32_t nits) {
    if (nits <= 50) {
      return 0;
    }
    double code = std::round(32.0 * std::log2(nits / 50.0));
    return (uint8_t) (code > 255 ? 255 : code);
  }

  inline double luminance_from_code(uint8_t code) {
    return 50.0 * std::pow(2.0, code / 32.0);
  }

  /// CTA-861.3 minimum luminance: max * (code / 255)^2 / 100 nits, with max decoded from its own code
  inline uint8_t min_luminance_code(uint32_t min_luminance, uint8_t max_code) {
    double max = luminance_from_code(max_code);
    double min = min_luminance / 10000.0;
    double code = std::round(255.0 * std::sqrt(min * 100.0 / max));
    return (uint8_t) (code > 255 ? 255 : code);
  }

  struct timing_t {
    uint32_t h_active, h_blank, h_front, h_sync;
    uint32_t v_active, v_blank, v_front, v_sync;
    /// In 10 kHz units, as a detailed timing descriptor stores it
    uint32_t pixel_clock_10khz;
  };

  /// CVT reduced blanking, close enough for a display no signal ever reaches
  inline timing_t reduced_blanking(uint32_t width, uint32_t height, uint32_t refresh_mhz) {
    timing_t t {};
    t.h_active = width;
    t.h_blank = 160;
    t.h_front = 48;
    t.h_sync = 32;
    t.v_active = height;
    t.v_front = 3;
    t.v_sync = 6;

    // At least 460 us of vertical blanking
    double frame_us = 1e9 / refresh_mhz;
    double line_us = (frame_us - 460.0) / height;
    uint32_t v_blank = line_us > 0 ? (uint32_t) (460.0 / line_us) + 1 : 0;
    t.v_blank = v_blank < 3 + 6 + 6 ? 3 + 6 + 6 : v_blank;

    double clock = (double) (width + t.h_blank) * (height + t.v_blank) * refresh_mhz / 1000.0;
    t.pixel_clock_10khz = (uint32_t) std::llround(clock / 10000.0);
    return t;
  }

  /// Whether a detailed timing descriptor, and the range limits below, can describe it
  inline bool dtd_fits(const timing_t &t) {
    if (t.h_active > 4095 || t.v_active > 4095 || t.h_blank > 4095 || t.v_blank > 4095) {
      return false;
    }
    if (t.pixel_clock_10khz == 0 || t.pixel_clock_10khz > 65535) {
      return false;
    }
    double h_khz = t.pixel_clock_10khz * 10.0 / (t.h_active + t.h_blank);
    double v_hz = h_khz * 1000.0 / (t.v_active + t.v_blank);
    return h_khz >= 20 && h_khz <= 255 && v_hz >= 15 && v_hz <= 255;
  }

  /// The preferred mode if a DTD can hold it, else the same size at 60 Hz, else 1920x1080 at 60 Hz.
  /// Windows takes the modes from the driver, not from these timings.
  inline timing_t preferred_timing(const LUZVDA_MODE &m) {
    auto t = reduced_blanking(m.width, m.height, m.refresh_mhz);
    if (dtd_fits(t)) {
      return t;
    }
    t = reduced_blanking(m.width, m.height, 60000);
    if (dtd_fits(t)) {
      return t;
    }
    return reduced_blanking(1920, 1080, 60000);
  }

  inline void write_dtd(uint8_t *d, const timing_t &t, uint16_t width_mm, uint16_t height_mm) {
    d[0] = t.pixel_clock_10khz & 0xff;
    d[1] = (t.pixel_clock_10khz >> 8) & 0xff;
    d[2] = t.h_active & 0xff;
    d[3] = t.h_blank & 0xff;
    d[4] = (uint8_t) (((t.h_active >> 8) << 4) | ((t.h_blank >> 8) & 0x0f));
    d[5] = t.v_active & 0xff;
    d[6] = t.v_blank & 0xff;
    d[7] = (uint8_t) (((t.v_active >> 8) << 4) | ((t.v_blank >> 8) & 0x0f));
    d[8] = t.h_front & 0xff;
    d[9] = t.h_sync & 0xff;
    d[10] = (uint8_t) (((t.v_front & 0x0f) << 4) | (t.v_sync & 0x0f));
    d[11] = (uint8_t) ((((t.h_front >> 8) & 3) << 6) | (((t.h_sync >> 8) & 3) << 4) | (((t.v_front >> 4) & 3) << 2) | ((t.v_sync >> 4) & 3));
    d[12] = (uint8_t) (width_mm & 0xff);
    d[13] = (uint8_t) (height_mm & 0xff);
    d[14] = (uint8_t) ((((width_mm >> 8) & 0x0f) << 4) | ((height_mm >> 8) & 0x0f));
    d[15] = 0;
    d[16] = 0;
    // Digital separate sync, +hsync -vsync, as reduced blanking expects
    d[17] = 0x1a;
  }

  /// A 13-character text descriptor: the text, then a line feed, then spaces
  inline void write_text_descriptor(uint8_t *d, uint8_t tag, const std::string &text) {
    std::memset(d, 0, 5);
    d[3] = tag;
    std::size_t n = text.size() < name_chars ? text.size() : name_chars;
    std::memcpy(d + 5, text.data(), n);
    if (n < name_chars) {
      d[5 + n] = 0x0a;
      for (std::size_t i = n + 1; i < name_chars; ++i) {
        d[5 + i] = ' ';
      }
    }
  }

  inline void write_checksum(uint8_t *block) {
    uint32_t sum = 0;
    for (int i = 0; i < 127; ++i) {
      sum += block[i];
    }
    block[127] = (uint8_t) (256 - sum % 256);
  }

  inline edid_t build(const input_t &in) {
    edid_t e {};
    uint8_t *b = e.data();

    // Header
    const uint8_t header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    std::memcpy(b, header, 8);

    uint16_t id = (uint16_t) (((manufacturer[0] - '@') << 10) | ((manufacturer[1] - '@') << 5) | (manufacturer[2] - '@'));
    b[8] = (uint8_t) (id >> 8);
    b[9] = (uint8_t) (id & 0xff);
    b[10] = product_code & 0xff;
    b[11] = product_code >> 8;
    b[12] = in.serial & 0xff;
    b[13] = (in.serial >> 8) & 0xff;
    b[14] = (in.serial >> 16) & 0xff;
    b[15] = (uint8_t) ((in.serial >> 24) & 0xff);

    // Fixed so the same display always gets the same bytes: week 1 of 2026
    b[16] = 1;
    b[17] = 2026 - 1990;

    // EDID 1.3, digital input
    b[18] = 1;
    b[19] = 3;
    b[20] = 0x80;
    b[21] = (uint8_t) ((in.width_mm + 5) / 10 > 255 ? 255 : (in.width_mm + 5) / 10);
    b[22] = (uint8_t) ((in.height_mm + 5) / 10 > 255 ? 255 : (in.height_mm + 5) / 10);
    // Gamma 2.2
    b[23] = 0x78;

    bool zero_primaries = true;
    for (auto p : in.primaries) {
      zero_primaries = zero_primaries && p == 0;
    }
    const auto &p = zero_primaries ? srgb_primaries : in.primaries;
    bool srgb = p == srgb_primaries;

    // RGB color, the first detailed timing is preferred, GTF; sRGB only when the primaries are sRGB's
    b[24] = 0x0b | (srgb ? 0x04 : 0x00);

    b[25] = (uint8_t) (((p[0] & 3) << 6) | ((p[1] & 3) << 4) | ((p[2] & 3) << 2) | (p[3] & 3));
    b[26] = (uint8_t) (((p[4] & 3) << 6) | ((p[5] & 3) << 4) | ((p[6] & 3) << 2) | (p[7] & 3));
    for (int i = 0; i < 8; ++i) {
      b[27 + i] = (uint8_t) (p[i] >> 2);
    }

    // Established and standard timings, as SudoVDA had them
    const uint8_t timings[19] = {
      0xa5, 0x6b, 0x80,
      0xd1, 0xc0, 0xb3, 0x00, 0xa9, 0xc0, 0x81, 0x80, 0x81, 0x00, 0x81, 0xc0, 0x01, 0x01, 0x01, 0x01,
    };
    std::memcpy(b + 35, timings, sizeof(timings));

    // Descriptor 1: the preferred timing
    write_dtd(b + 54, preferred_timing(in.preferred), in.width_mm, in.height_mm);

    // Descriptor 2: the serial as text, or the rest of a long name
    std::string name = in.name.empty() ? std::string("LuzVDA") : in.name;
    char serial_text[16];
    static const char hex[] = "0123456789ABCDEF";
    serial_text[0] = 'L';
    serial_text[1] = 'V';
    for (int i = 0; i < 8; ++i) {
      serial_text[2 + i] = hex[(in.serial >> (28 - 4 * i)) & 0xf];
    }
    bool split = in.split_name && name.size() > name_chars;
    if (split) {
      write_text_descriptor(b + 72, 0xfc, name);
    } else {
      write_text_descriptor(b + 72, 0xff, std::string(serial_text, 10));
    }

    // Descriptor 3: range limits, 15-255 Hz, 20-255 kHz, up to 2550 MHz, as SudoVDA had them
    const uint8_t range[18] = {0x00, 0x00, 0x00, 0xfd, 0x00, 0x0f, 0xff, 0x14, 0xff, 0xff, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20};
    std::memcpy(b + 90, range, sizeof(range));

    // Descriptor 4: the name
    write_text_descriptor(b + 108, 0xfc, split ? name.substr(name_chars) : name);

    // One extension
    b[126] = 1;
    write_checksum(b);

    // CTA-861 extension, revision 3: underscan, basic audio, YCbCr 4:4:4 and 4:2:2
    uint8_t *c = b + 128;
    c[0] = 0x02;
    c[1] = 0x03;
    c[3] = 0xf0;

    std::vector<uint8_t> blocks = {
      // Video: SudoVDA's list of 2160p24/25/30/50/60 and common TV modes, with 1080p60 moved first.
      // edid-decode reads the first one as preferred over the detailed timing; 1080p60 is a safer
      // reading than SudoVDA's 2160p24. Windows takes an indirect display's modes from the driver.
      0x51, 0x10, 0x5d, 0x5e, 0x5f, 0x60, 0x61, 0x1f, 0x22, 0x21, 0x20, 0x05, 0x14, 0x04, 0x13, 0x12, 0x03, 0x01,
      // Audio: 8-channel LPCM
      0x23, 0x0f, 0x56, 0x05,
      // Speaker allocation
      0x83, 0x0f, 0x08, 0x00,
      // HDMI 1.4 VSDB: 1.0.0.0, deep color 30/36-bit and 4:4:4, 340 MHz (its maximum), HDMI VICs 1-3
      0x6d, 0x03, 0x0c, 0x00, 0x10, 0x00, 0x38, 0x44, 0x20, 0x00, 0x60, 0x01, 0x02, 0x03,
      // HDMI Forum VSDB: 600 MHz, SCDC, 4:2:0 deep color
      0x67, 0xd8, 0x5d, 0xc4, 0x01, 0x78, 0x80, 0x03,
      // Video capability: RGB and YCbCr quantization range selectable, IT and CE formats underscanned
      0xe2, 0x00, 0xca,
    };

    if (in.hdr) {
      // Colorimetry: BT.2020 cYCC, YCC and RGB
      const uint8_t colorimetry[] = {0xe3, 0x05, 0xe0, 0x00};
      blocks.insert(blocks.end(), colorimetry, colorimetry + sizeof(colorimetry));
    }

    // YCbCr 4:2:0 capability map: 2160p50 and 2160p60, the 5th and 6th video formats
    const uint8_t ycbcr420[] = {0xe4, 0x0f, 0x30, 0x00, 0x00};
    blocks.insert(blocks.end(), ycbcr420, ycbcr420 + sizeof(ycbcr420));

    if (in.hdr) {
      // HDR static metadata: SDR and HDR gamma, PQ and HLG; type 1; the screen's own luminance
      uint8_t max_code = luminance_code(in.max_nits);
      uint8_t full_code = luminance_code(in.max_full_frame_nits ? in.max_full_frame_nits : in.max_nits);
      uint8_t min_code = min_luminance_code(in.min_luminance, max_code);
      const uint8_t hdr[] = {0xe6, 0x06, 0x0f, 0x01, max_code, full_code, min_code};
      blocks.insert(blocks.end(), hdr, hdr + sizeof(hdr));
    }

    std::memcpy(c + 4, blocks.data(), blocks.size());
    c[2] = (uint8_t) (4 + blocks.size());
    write_checksum(c);

    return e;
  }
}  // namespace luzvda::edid
