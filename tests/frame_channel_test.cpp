// Tests for the frame channel's shared rules in include/luzvda/luzvda-frames.h: object names, when
// the driver copies, and which slot it fills.
//   g++ -std=c++17 -Wall -Wextra -Iinclude tests/frame_channel_test.cpp -o frame_channel_test

#include "luzvda/luzvda-frames.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace luzvda::frames;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static void test_names() {
  // {6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60} as Windows lays out a GUID: the first three fields little-endian
  LUZVDA_GUID guid = {{0x1e, 0x9c, 0x0b, 0x6a, 0x4d, 0x3f, 0x2a, 0x4c, 0x9e, 0x7b, 0x1d, 0x2c, 0x3b, 0x4a, 0x5f, 0x60}};
  std::string prefix = prefix_for(guid);
  CHECK(prefix == "Global\\LuzVDA-6a0b9c1e-3f4d-4c2a-9e7b-1d2c3b4a5f60");
  CHECK(prefix.size() < LUZVDA_FRAME_PREFIX_BYTES);
  CHECK(header_name(prefix) == prefix + "-header");
  CHECK(ready_name(prefix) == prefix + "-ready");
  CHECK(slot_name(prefix, 12, 2) == prefix + "-g12-s2");

  // Every texture name differs across generations and slots
  std::set<std::string> seen;
  for (uint32_t g = 1; g <= 20; ++g) {
    for (uint32_t s = 0; s < LUZVDA_FRAME_SLOTS; ++s) {
      CHECK(seen.insert(slot_name(prefix, g, s)).second);
    }
  }
}

static void test_latest() {
  CHECK(pack_latest(0, 0) == 0);
  uint64_t latest = pack_latest(123456789, 2);
  CHECK(LUZVDA_FRAME_LATEST_FRAME(latest) == 123456789);
  CHECK(LUZVDA_FRAME_LATEST_SLOT(latest) == 2);
}

static void test_consumer_active() {
  const int64_t f = 10000000;  // QueryPerformanceFrequency on most PCs
  CHECK(!consumer_active(5 * f, 0, f));  // never written
  CHECK(consumer_active(5 * f, 5 * f, f));
  CHECK(consumer_active(5 * f, 3 * f + 1, f));  // just under 2 s
  CHECK(!consumer_active(5 * f, 3 * f, f));  // 2 s
  CHECK(!consumer_active(100 * f, 5 * f, f));
  CHECK(consumer_active(5 * f, 6 * f, f));  // a consumer a little ahead of the driver
  CHECK(!consumer_active(5 * f, 5 * f, 0));
}

static void test_pick_slot() {
  // No frame yet: the first free slot from 0
  std::vector<uint32_t> tried;
  int slot = pick_slot(0, 3, [&](uint32_t i) {
    tried.push_back(i);
    return true;
  });
  CHECK(slot == 0);
  CHECK(tried.size() == 1);

  // Never the newest slot, and the one after it first
  tried.clear();
  slot = pick_slot(pack_latest(7, 1), 3, [&](uint32_t i) {
    tried.push_back(i);
    return true;
  });
  CHECK(slot == 2);

  tried.clear();
  slot = pick_slot(pack_latest(7, 2), 3, [&](uint32_t i) {
    tried.push_back(i);
    return i != 0;  // the consumer holds slot 0
  });
  CHECK(slot == 1);
  CHECK(tried.size() == 2 && tried[0] == 0 && tried[1] == 1);

  // The newest slot isn't tried even when the others are busy
  tried.clear();
  slot = pick_slot(pack_latest(9, 0), 3, [&](uint32_t i) {
    tried.push_back(i);
    return false;
  });
  CHECK(slot == -1);
  CHECK(tried.size() == 2);
  for (uint32_t i : tried) {
    CHECK(i != 0);
  }

  // A run of frames with one consumer holding a slot at a time: the newest is always readable
  uint64_t latest = 0;
  int held = -1;
  for (uint64_t frame = 1; frame <= 50; ++frame) {
    held = latest ? (int) LUZVDA_FRAME_LATEST_SLOT(latest) : -1;  // the consumer takes the newest
    slot = pick_slot(latest, 3, [&](uint32_t i) {
      return (int) i != held;
    });
    CHECK(slot >= 0 && slot != held);
    latest = pack_latest(frame, (uint32_t) slot);
  }
}

static void test_layout() {
  CHECK(sizeof(LUZVDA_FRAME_HEADER) == 192);
  CHECK(sizeof(LUZVDA_FRAME_CHANNEL) == 88);
  CHECK(LUZVDA_FRAME_SLOTS <= 8);  // the names field has a bit per slot below LUZVDA_FRAME_NAMED_READY
  CHECK((LUZVDA_FRAME_NAMED_READY & ((1u << LUZVDA_FRAME_SLOTS) - 1)) == 0);
  CHECK(IOCTL_LUZVDA_OPEN_FRAME_CHANNEL == 0x0022E054u);
  CHECK(LUZVDA_PROTOCOL_MINOR == 1);
}

int main() {
  test_names();
  test_latest();
  test_consumer_active();
  test_pick_slot();
  test_layout();

  if (s_Failures) {
    std::printf("%d frame channel checks failed\n", s_Failures);
    return 1;
  }
  std::printf("All frame channel tests passed\n");
  return 0;
}
