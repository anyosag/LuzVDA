// Tests for tools/luzvda-ctl/frame_code.h: the code the capture test draws and reads back.
//   g++ -std=c++17 -Wall -Wextra -Itools tests/frame_code_test.cpp -o frame_code_test

#include "luzvda-ctl/frame_code.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace luzvda::frame_code;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

// A frame in memory, black, with the code for value drawn as the capture test's window draws it
struct image_t {
  int width;
  int height;
  uint32_t format;
  std::vector<uint8_t> pixels;

  image_t(int w, int h, uint32_t f) : width(w), height(h), format(f), pixels((size_t) w * h * bytes_per_pixel(f), 0) {}

  uint8_t *at(int x, int y) {
    return pixels.data() + ((size_t) y * width + x) * bytes_per_pixel(format);
  }

  void fill(const rect_t &r, float level) {
    for (int y = r.top; y < r.bottom; ++y) {
      for (int x = r.left; x < r.right; ++x) {
        uint8_t *p = at(x, y);
        if (format == k_r16g16b16a16_float) {
          // level as a half: 1.0 = 0x3C00, 2.5 = 0x4100, 0 = 0
          uint16_t h = level == 0 ? 0 : (level == 1.0f ? 0x3C00 : 0x4100);
          for (int c = 0; c < 3; ++c) {
            std::memcpy(p + 2 * c, &h, 2);
          }
        } else if (format == k_r10g10b10a2_unorm) {
          uint32_t v = (uint32_t) (level > 0 ? 0x3FF : 0);
          uint32_t packed = v | v << 10 | v << 20 | 3u << 30;
          std::memcpy(p, &packed, 4);
        } else {
          uint8_t v = level > 0 ? 0xFF : 0;
          p[0] = p[1] = p[2] = v;
          p[3] = 0xFF;
        }
      }
    }
  }

  void draw(uint64_t value, float white) {
    for (const auto &r : white_blocks(value)) {
      fill(r, white);
    }
  }

  bool read(uint64_t &value) {
    return decode([&](int x, int y) {
      return is_light(format, at(x, y));
    }, value);
  }
};

static void test_round_trip() {
  const uint32_t formats[] = {k_b8g8r8a8_unorm, k_r8g8b8a8_unorm, k_b8g8r8x8_unorm, k_r10g10b10a2_unorm, k_r16g16b16a16_float};
  const uint64_t values[] = {0, 1, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull, 0x0000123456789ABCull, 1234567890123ull};
  for (uint32_t format : formats) {
    for (uint64_t value : values) {
      image_t image(k_left + k_width + 16, k_top + k_height + 16, format);
      image.draw(value, format == k_r16g16b16a16_float ? 2.5f : 1.0f);
      uint64_t read = 42;
      CHECK(image.read(read));
      CHECK(read == value);
    }
  }

  // SDR white in an HDR frame can be 1.0 (80 nits) as well as above
  image_t sdr_white(k_left + k_width, k_top + k_height, k_r16g16b16a16_float);
  sdr_white.draw(0xDEADBEEFull, 1.0f);
  uint64_t read = 0;
  CHECK(sdr_white.read(read) && read == 0xDEADBEEFull);
}

static void test_rejects() {
  // All black or all white isn't a value and its complement
  image_t black(k_left + k_width, k_top + k_height, k_b8g8r8a8_unorm);
  uint64_t read = 7;
  CHECK(!black.read(read));
  CHECK(read == 7);

  image_t white(k_left + k_width, k_top + k_height, k_b8g8r8a8_unorm);
  white.fill({0, 0, white.width, white.height}, 1.0f);
  CHECK(!white.read(read));

  // A frame torn between two values: the top half from one, the bottom from another
  image_t a(k_left + k_width, k_top + k_height, k_b8g8r8a8_unorm);
  image_t b(k_left + k_width, k_top + k_height, k_b8g8r8a8_unorm);
  a.draw(0x1111111111111111ull, 1.0f);
  b.draw(0x2222222222222222ull, 1.0f);
  size_t half = a.pixels.size() / 2;
  std::memcpy(a.pixels.data() + half, b.pixels.data() + half, a.pixels.size() - half);
  CHECK(!a.read(read));

  // One block wrong
  image_t one(k_left + k_width, k_top + k_height, k_b8g8r8a8_unorm);
  one.draw(0x5555555555555555ull, 1.0f);
  one.fill(block(5), bit(0x5555555555555555ull, 5) ? 0.0f : 1.0f);
  CHECK(!one.read(read));
}

static void test_layout() {
  // Blocks don't overlap and stay inside the code's area
  for (int i = 0; i < k_bits; ++i) {
    rect_t r = block(i);
    CHECK(r.right - r.left == k_block && r.bottom - r.top == k_block);
    CHECK(r.left >= k_left && r.right <= k_left + k_width);
    CHECK(r.top >= k_top && r.bottom <= k_top + k_height);
    for (int j = i + 1; j < k_bits; ++j) {
      rect_t s = block(j);
      CHECK(r.right <= s.left || s.right <= r.left || r.bottom <= s.top || s.bottom <= r.top);
    }
  }
  CHECK(k_columns * k_rows == k_bits);
  // Exactly half the blocks are white for any value
  CHECK(white_blocks(0x0123456789ABCDEFull).size() == 64);
}

static void test_half() {
  CHECK(half_to_float(0x0000) == 0.0f);
  CHECK(half_to_float(0x3C00) == 1.0f);
  CHECK(half_to_float(0x4100) == 2.5f);
  CHECK(half_to_float(0xC000) == -2.0f);
  CHECK(half_to_float(0x7BFF) == 65504.0f);
  CHECK(std::fabs(half_to_float(0x0001) - 5.9604645e-8f) < 1e-12f);  // smallest subnormal
  CHECK(std::fabs(half_to_float(0x03FF) - 6.0975552e-5f) < 1e-10f);  // largest subnormal
  CHECK(std::isinf(half_to_float(0x7C00)));
  CHECK(bytes_per_pixel(k_r16g16b16a16_float) == 8 && bytes_per_pixel(k_b8g8r8a8_unorm) == 4 && bytes_per_pixel(0) == 0);
}

int main() {
  test_round_trip();
  test_rejects();
  test_layout();
  test_half();

  if (s_Failures) {
    std::printf("%d frame code checks failed\n", s_Failures);
    return 1;
  }
  std::printf("All frame code tests passed\n");
  return 0;
}
