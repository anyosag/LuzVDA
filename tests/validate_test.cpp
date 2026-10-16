// Tests for src/common/validate.h: what the driver accepts from callers.
//   g++ -std=c++17 -Wall -Wextra -Iinclude -Isrc tests/validate_test.cpp -o validate_test

#include "common/validate.h"

#include <cstdio>
#include <cstring>

using namespace luzvda;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static LUZVDA_DISPLAY_DESC good() {
  LUZVDA_DISPLAY_DESC d;
  std::memset(&d, 0, sizeof(d));
  d.struct_size = sizeof(d);
  d.flags = LUZVDA_DISPLAY_HDR;
  d.monitor_guid.bytes[0] = 1;
  std::strcpy(d.name, "MacBook Pro");
  d.width_mm = 302;
  d.height_mm = 196;
  const uint16_t p3[8] = {696, 328, 271, 707, 154, 61, 320, 337};
  std::memcpy(d.primaries, p3, sizeof(p3));
  d.max_luminance = 1600;
  d.max_full_frame_luminance = 1000;
  d.min_luminance = 50;
  d.mode_count = 2;
  d.preferred_mode = 1;
  d.modes[0] = {3024, 1964, 120000};
  d.modes[1] = {3024, 1890, 120000};
  return d;
}

static void test_desc() {
  auto d = good();
  CHECK(check(d, sizeof(d)) == check_e::ok);
  CHECK(check(d, sizeof(d) - 1) == check_e::too_small);

  d = good();
  d.struct_size = 56;
  CHECK(check(d, sizeof(d)) == check_e::bad_struct_size);

  d = good();
  d.flags |= 0x80;
  CHECK(check(d, sizeof(d)) == check_e::bad_flags);

  // A name that fills the array has no NUL
  d = good();
  std::memset(d.name, 'A', sizeof(d.name));
  CHECK(check(d, sizeof(d)) == check_e::bad_name);

  d = good();
  d.mode_count = 0;
  CHECK(check(d, sizeof(d)) == check_e::bad_mode_count);
  d.mode_count = LUZVDA_MAX_MODES + 1;
  CHECK(check(d, sizeof(d)) == check_e::bad_mode_count);

  d = good();
  d.preferred_mode = 2;
  CHECK(check(d, sizeof(d)) == check_e::bad_preferred);

  d = good();
  d.modes[0].width = 0;
  CHECK(check(d, sizeof(d)) == check_e::bad_mode);
  d = good();
  d.modes[0].height = 9000;
  CHECK(check(d, sizeof(d)) == check_e::bad_mode);
  d = good();
  d.modes[0].refresh_mhz = 120;  // hertz, not millihertz
  CHECK(check(d, sizeof(d)) == check_e::bad_mode);
  d = good();
  d.modes[0].refresh_mhz = 1000000;
  CHECK(check(d, sizeof(d)) == check_e::bad_mode);
  d = good();
  d.modes[0] = {8192, 8192, 240000};  // too many pixels per second
  CHECK(check(d, sizeof(d)) == check_e::bad_mode);
  d = good();
  d.modes[0] = {7680, 4320, 240000};
  CHECK(check(d, sizeof(d)) == check_e::ok);

  d = good();
  d.width_mm = 2001;
  CHECK(check(d, sizeof(d)) == check_e::bad_size_mm);

  d = good();
  d.primaries[0] = 1024;
  CHECK(check(d, sizeof(d)) == check_e::bad_primaries);
  d = good();
  d.primaries[6] = 0;
  CHECK(check(d, sizeof(d)) == check_e::bad_primaries);
  d = good();
  std::memset(d.primaries, 0, sizeof(d.primaries));
  CHECK(check(d, sizeof(d)) == check_e::ok);

  d = good();
  d.max_luminance = 0;
  CHECK(check(d, sizeof(d)) == check_e::bad_luminance);
  d = good();
  d.max_full_frame_luminance = 2000;
  CHECK(check(d, sizeof(d)) == check_e::bad_luminance);
  d = good();
  d.max_luminance = 20000;
  CHECK(check(d, sizeof(d)) == check_e::bad_luminance);
  d = good();
  d.min_luminance = 1600 * 10000;
  CHECK(check(d, sizeof(d)) == check_e::bad_luminance);

  // SDR ignores luminance
  d = good();
  d.flags = 0;
  d.max_luminance = 0;
  CHECK(check(d, sizeof(d)) == check_e::ok);
}

static void test_update() {
  LUZVDA_UPDATE_MODES u;
  std::memset(&u, 0, sizeof(u));
  u.struct_size = sizeof(u);
  u.mode_count = 1;
  u.modes[0] = {1920, 1080, 60000};
  CHECK(check(u, sizeof(u)) == check_e::ok);
  CHECK(check(u, sizeof(u) - 1) == check_e::too_small);
  u.preferred_mode = 1;
  CHECK(check(u, sizeof(u)) == check_e::bad_preferred);
}

static void test_mode_list() {
  LUZVDA_MODE modes[4] = {{3024, 1964, 120000}, {3024, 1890, 120000}, {3024, 1964, 120000}, {3024, 1890, 60000}};

  // Repeats dropped, the preferred one followed, and the fallback added
  auto l = mode_list(4, 2, modes);
  CHECK(l.modes.size() == 4);
  CHECK(l.preferred == 0);
  CHECK(l.modes[3].width == 1920 && l.modes[3].refresh_mhz == 60000);

  l = mode_list(4, 3, modes);
  CHECK(l.preferred == 2);

  // No second fallback
  LUZVDA_MODE fhd[1] = {{1920, 1080, 60000}};
  l = mode_list(1, 0, fhd);
  CHECK(l.modes.size() == 1 && l.preferred == 0);
}

static void test_names() {
  CHECK(ascii_name("MacBook Pro", 64) == "MacBook Pro");
  CHECK(ascii_name("Caf\xc3\xa9 Mac", 64) == "Caf Mac");
  CHECK(ascii_name("tab\there", 64) == "tabhere");
  const char unterminated[4] = {'a', 'b', 'c', 'd'};
  CHECK(ascii_name(unterminated, 4) == "abcd");
}

int main() {
  test_desc();
  test_update();
  test_mode_list();
  test_names();

  if (s_Failures) {
    std::printf("%d validation checks failed\n", s_Failures);
    return 1;
  }
  std::printf("All validation tests passed\n");
  return 0;
}
