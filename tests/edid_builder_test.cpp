// Tests for src/edid/edid_builder.h. Writes the EDIDs it builds to the folder given as its argument,
// for edid-decode --check:
//   g++ -std=c++17 -Wall -Wextra -Iinclude -Isrc tests/edid_builder_test.cpp -o edid_builder_test
//   ./edid_builder_test out && for f in out/*.bin; do edid-decode --check "$f"; done

#include "edid/edid_builder.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace luzvda::edid;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static bool checksum_ok(const uint8_t *block) {
  uint32_t sum = 0;
  for (int i = 0; i < 128; ++i) {
    sum += block[i];
  }
  return sum % 256 == 0;
}

static uint16_t chroma(const edid_t &e, int i) {
  // High 8 bits at 27 + i, low 2 bits packed into bytes 25 and 26
  int lo_byte = i < 4 ? 25 : 26;
  int shift = 6 - 2 * (i % 4);
  return (uint16_t) ((e[27 + i] << 2) | ((e[lo_byte] >> shift) & 3));
}

static std::string text_at(const edid_t &e, int offset) {
  std::string s;
  for (int i = 0; i < 13 && e[offset + 5 + i] != 0x0a; ++i) {
    s.push_back((char) e[offset + 5 + i]);
  }
  return s;
}

// A CTA data block by tag (extended tag for tag 7), or -1
static int find_block(const edid_t &e, int tag, int ext_tag = -1) {
  int end = 128 + e[130];
  for (int i = 132; i < end;) {
    int t = e[i] >> 5;
    int len = e[i] & 0x1f;
    if (t == tag && (ext_tag < 0 || e[i + 1] == ext_tag)) {
      return i;
    }
    i += 1 + len;
  }
  return -1;
}

static input_t macbook_pro_14() {
  input_t in;
  const uint8_t guid[16] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
  in.serial = serial_for(guid);
  in.name = "MacBook Pro 14 of someone";
  in.width_mm = 302;
  in.height_mm = 196;
  // Display P3, D65
  in.primaries = {696, 328, 271, 707, 154, 61, 320, 337};
  in.hdr = true;
  in.max_nits = 1600;
  in.max_full_frame_nits = 1000;
  in.min_luminance = 50;  // 0.005 nits
  in.preferred = {3024, 1964, 120000};
  return in;
}

static void test_codes() {
  CHECK(luminance_code(1600) == 160);
  CHECK(luminance_code(1000) == 138);
  CHECK(luminance_code(50) == 0);
  CHECK(luminance_code(0) == 0);
  CHECK(luminance_code(10000) == 245);
  CHECK(std::fabs(luminance_from_code(160) - 1600.0) < 0.01);
  CHECK(min_luminance_code(0, 160) == 0);
  CHECK(min_luminance_code(50, 160) == 5);

  const uint8_t a[16] = {1};
  const uint8_t b[16] = {2};
  CHECK(serial_for(a) != serial_for(b));
  CHECK(serial_for(a) != 0);
}

static void test_macbook() {
  auto in = macbook_pro_14();
  auto e = build(in);

  CHECK(checksum_ok(e.data()));
  CHECK(checksum_ok(e.data() + 128));

  // LZV
  CHECK(e[8] == 0x33 && e[9] == 0x56);
  CHECK((uint32_t) (e[12] | e[13] << 8 | e[14] << 16 | (uint32_t) e[15] << 24) == in.serial);

  // 30 x 20 cm
  CHECK(e[21] == 30 && e[22] == 20);

  // P3 primaries, sRGB bit off
  for (int i = 0; i < 8; ++i) {
    CHECK(chroma(e, i) == in.primaries[i]);
  }
  CHECK((e[24] & 0x04) == 0);

  // 3024x1964 at 120 Hz is too fast for a DTD, so it says 60 Hz, with the size in mm
  int width = e[56] | (e[58] >> 4) << 8;
  int height = e[59] | (e[61] >> 4) << 8;
  CHECK(width == 3024 && height == 1964);
  CHECK((e[66] | (e[68] >> 4) << 8) == 302);
  CHECK((e[67] | (e[68] & 0x0f) << 8) == 196);

  CHECK(text_at(e, 108) == "MacBook Pro 1");
  CHECK(e[108 + 3] == 0xfc);
  CHECK(e[72 + 3] == 0xff);

  int hdr = find_block(e, 7, 6);
  CHECK(hdr > 0);
  if (hdr > 0) {
    CHECK(e[hdr + 4] == 160);
    CHECK(e[hdr + 5] == 138);
    CHECK(e[hdr + 6] == 5);
  }
  CHECK(find_block(e, 7, 5) > 0);
  CHECK(find_block(e, 7, 0) > 0);
}

static void test_sdr() {
  input_t in;
  in.name = "";
  in.preferred = {1920, 1080, 60000};
  auto e = build(in);

  CHECK(checksum_ok(e.data()));
  CHECK(checksum_ok(e.data() + 128));
  CHECK(text_at(e, 108) == "LuzVDA");

  // All-zero primaries mean sRGB, with its bit set
  for (int i = 0; i < 8; ++i) {
    CHECK(chroma(e, i) == srgb_primaries[i]);
  }
  CHECK((e[24] & 0x04) != 0);

  // Unknown size
  CHECK(e[21] == 0 && e[22] == 0);

  // No HDR blocks
  CHECK(find_block(e, 7, 6) < 0);
  CHECK(find_block(e, 7, 5) < 0);
}

static void test_split_name() {
  auto in = macbook_pro_14();
  in.split_name = true;
  auto e = build(in);
  CHECK(checksum_ok(e.data()));
  CHECK(e[72 + 3] == 0xfc);
  CHECK(text_at(e, 72) == "MacBook Pro 1");
  CHECK(text_at(e, 108) == "4 of someone");

  // A short name keeps the serial text
  in.name = "Short";
  e = build(in);
  CHECK(e[72 + 3] == 0xff);
}

static void test_same_input_same_bytes() {
  CHECK(build(macbook_pro_14()) == build(macbook_pro_14()));
  auto other = macbook_pro_14();
  other.serial ^= 1;
  CHECK(build(other) != build(macbook_pro_14()));
}

static void test_dtd_fallbacks() {
  // Fits as asked
  auto t = preferred_timing({1920, 1080, 144000});
  CHECK(t.h_active == 1920 && dtd_fits(t));
  double hz = t.pixel_clock_10khz * 10000.0 / ((t.h_active + t.h_blank) * (t.v_active + t.v_blank));
  CHECK(std::fabs(hz - 144.0) < 0.5);

  // 8K doesn't fit a DTD at all
  t = preferred_timing({7680, 4320, 60000});
  CHECK(t.h_active == 1920 && t.v_active == 1080);

  // 4K at 240 Hz drops to 60 Hz
  t = preferred_timing({3840, 2160, 240000});
  CHECK(t.h_active == 3840 && dtd_fits(t));
}

static void write(const std::string &dir, const char *name, const edid_t &e) {
  std::string path = dir + "/" + name + ".bin";
  if (FILE *f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(e.data(), 1, e.size(), f);
    std::fclose(f);
  } else {
    std::printf("FAIL can't write %s\n", path.c_str());
    s_Failures++;
  }
}

int main(int argc, char **argv) {
  test_codes();
  test_macbook();
  test_sdr();
  test_split_name();
  test_same_input_same_bytes();
  test_dtd_fallbacks();

  if (argc > 1) {
    std::string dir = argv[1];
    write(dir, "macbook-pro-14-hdr", build(macbook_pro_14()));

    auto split = macbook_pro_14();
    split.split_name = true;
    write(dir, "split-name", build(split));

    input_t sdr;
    sdr.name = "Office laptop";
    sdr.width_mm = 344;
    sdr.height_mm = 194;
    sdr.preferred = {1920, 1080, 60000};
    write(dir, "laptop-sdr", build(sdr));

    input_t tv;
    tv.name = "Living room TV";
    tv.width_mm = 1210;
    tv.height_mm = 680;
    tv.hdr = true;
    tv.max_nits = 800;
    tv.max_full_frame_nits = 400;
    tv.min_luminance = 100;
    tv.preferred = {3840, 2160, 120000};
    write(dir, "tv-4k-hdr", build(tv));

    input_t apollo_default;
    apollo_default.name = "ApolloDisplay";
    apollo_default.width_mm = 700;
    apollo_default.height_mm = 390;
    apollo_default.hdr = true;
    apollo_default.max_nits = 3805;
    apollo_default.max_full_frame_nits = 3805;
    apollo_default.preferred = {2560, 1440, 59940};
    write(dir, "apollo-default", build(apollo_default));
  }

  if (s_Failures) {
    std::printf("%d EDID checks failed\n", s_Failures);
    return 1;
  }
  std::printf("All EDID builder tests passed\n");
  return 0;
}
