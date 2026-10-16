// The code luzvda-ctl's capture test draws on a display and reads back from captured frames: a
// 64-bit value (the QueryPerformanceCounter time just before the frame was presented) and its
// complement, as 128 black or white blocks. A frame that is torn between two values, scaled, or
// covered doesn't decode, rather than decoding wrong.
//
// Pure C++17 so that tests/frame_code_test.cpp runs anywhere.
//
// SPDX-License-Identifier: MS-PL

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace luzvda::frame_code {
  // In the display's pixels, from its top-left corner
  constexpr int k_columns = 16;
  constexpr int k_rows = 8;
  constexpr int k_block = 24;
  constexpr int k_left = 64;
  constexpr int k_top = 64;
  constexpr int k_width = k_columns * k_block;
  constexpr int k_height = k_rows * k_block;
  constexpr int k_bits = 128;

  struct rect_t {
    int left;
    int top;
    int right;
    int bottom;
  };

  // Bits 0-63 are the value, 64-127 its complement
  inline bool bit(uint64_t value, int i) {
    return i < 64 ? ((value >> i) & 1) != 0 : ((value >> (i - 64)) & 1) == 0;
  }

  inline rect_t block(int i) {
    int column = i % k_columns;
    int row = i / k_columns;
    return {k_left + column * k_block, k_top + row * k_block, k_left + (column + 1) * k_block, k_top + (row + 1) * k_block};
  }

  // The blocks to draw white for value; everything else in the code's area is black
  inline std::vector<rect_t> white_blocks(uint64_t value) {
    std::vector<rect_t> out;
    for (int i = 0; i < k_bits; ++i) {
      if (bit(value, i)) {
        out.push_back(block(i));
      }
    }
    return out;
  }

  // Reads a value back. is_light(x, y) says whether the display's pixel at x, y is light; each
  // block is read at its centre.
  template <class IsLight>
  bool decode(IsLight &&is_light, uint64_t &value) {
    uint64_t v = 0;
    uint64_t complement = 0;
    for (int i = 0; i < k_bits; ++i) {
      rect_t r = block(i);
      if (!is_light((r.left + r.right) / 2, (r.top + r.bottom) / 2)) {
        continue;
      }
      if (i < 64) {
        v |= uint64_t(1) << i;
      } else {
        complement |= uint64_t(1) << (i - 64);
      }
    }
    if (v != ~complement) {
      return false;
    }
    value = v;
    return true;
  }

  // DXGI_FORMAT values the capture paths produce
  constexpr uint32_t k_r16g16b16a16_float = 10;
  constexpr uint32_t k_r10g10b10a2_unorm = 24;
  constexpr uint32_t k_r8g8b8a8_unorm = 28;
  constexpr uint32_t k_b8g8r8a8_unorm = 87;
  constexpr uint32_t k_b8g8r8x8_unorm = 88;

  inline int bytes_per_pixel(uint32_t format) {
    switch (format) {
      case k_r16g16b16a16_float:
        return 8;
      case k_r10g10b10a2_unorm:
      case k_r8g8b8a8_unorm:
      case k_b8g8r8a8_unorm:
      case k_b8g8r8x8_unorm:
        return 4;
      default:
        return 0;
    }
  }

  inline float half_to_float(uint16_t h) {
    uint32_t sign = uint32_t(h >> 15) << 31;
    int exponent = (h >> 10) & 0x1F;
    uint32_t mantissa = h & 0x3FF;
    uint32_t bits;
    if (exponent == 0) {
      if (mantissa == 0) {
        bits = sign;
      } else {
        // Subnormal: normalize it
        int e = -1;
        do {
          ++e;
          mantissa <<= 1;
        } while ((mantissa & 0x400) == 0);
        bits = sign | uint32_t(127 - 15 - e) << 23 | (mantissa & 0x3FF) << 13;
      }
    } else if (exponent == 31) {
      bits = sign | 0x7F800000u | mantissa << 13;
    } else {
      bits = sign | uint32_t(exponent - 15 + 127) << 23 | mantissa << 13;
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
  }

  // Whether a pixel is closer to white than black, from its green channel. In HDR, Windows writes
  // white from an SDR program at its SDR white level (1.0 or more) and black at 0.
  inline bool is_light(uint32_t format, const uint8_t *pixel) {
    switch (format) {
      case k_r16g16b16a16_float: {
        uint16_t g;
        std::memcpy(&g, pixel + 2, sizeof(g));
        return half_to_float(g) > 0.25f;
      }
      case k_r10g10b10a2_unorm: {
        uint32_t p;
        std::memcpy(&p, pixel, sizeof(p));
        return ((p >> 10) & 0x3FF) > 0x200;
      }
      case k_r8g8b8a8_unorm:
      case k_b8g8r8a8_unorm:
      case k_b8g8r8x8_unorm:
        return pixel[1] > 0x80;
      default:
        return false;
    }
  }
}  // namespace luzvda::frame_code
