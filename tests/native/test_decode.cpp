// Host-compiled unit tests for the pure decode/encode helpers in decode.h.
//
// These run on the build host (no ESPHome / VitoWiFi), exactly the logic that
// turns raw Optolink bytes into ESPHome state and back. Build & run from the
// repo root (the include is relative, so no -I is needed):
//   g++ -std=c++17 -Wall -Wextra -Werror -o /tmp/test_decode tests/native/test_decode.cpp
//   /tmp/test_decode
//
// The CI workflow compiles and runs this on every push.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Relative include so this compiles with a bare `g++ tests/native/test_decode.cpp`
// (no -I needed): the CI workflow invokes g++ without an include path.
#include "../../components/vitohome/decode.h"

using namespace esphome::vitohome;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    ++g_checks;                                                   \
    if (!(cond)) {                                                \
      ++g_failures;                                               \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                             \
  } while (0)

static bool close_to(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

// --- read_le ---------------------------------------------------------------
static void test_read_le() {
  const uint8_t one[] = {0x2A};
  CHECK(read_le(one, 1) == 0x2A);

  // Little-endian: data[1]<<8 | data[0].
  const uint8_t two[] = {0x34, 0x12};
  CHECK(read_le(two, 2) == 0x1234);

  const uint8_t four[] = {0x78, 0x56, 0x34, 0x12};
  CHECK(read_le(four, 4) == 0x12345678ULL);

  // Precision point: a 4-byte counter past 2**24 must read EXACTLY (this is
  // why decode happens in uint64/double rather than float32). 212,197,680 s
  // is the burner-seconds behind ~58943.8 h.
  const uint32_t secs = 212197680u;  // 0x0CA63DB0
  const uint8_t le[] = {0x30, 0xE1, 0xA5, 0x0C};
  CHECK(read_le(le, 4) == secs);
}

// --- sign_extend_le --------------------------------------------------------
static void test_sign_extend() {
  // 0xFFD8 as int16 == -40 (the frosty-morning case the old unsigned decode
  // would have shown as 65496).
  CHECK(sign_extend_le(0xFFD8u, 2) == -40);
  CHECK(sign_extend_le(0x0028u, 2) == 40);
  // 1-byte signed.
  CHECK(sign_extend_le(0xF3u, 1) == -13);
  CHECK(sign_extend_le(0x7Fu, 1) == 127);
  CHECK(sign_extend_le(0x80u, 1) == -128);
}

// --- decode_scaled ---------------------------------------------------------
static void test_decode_scaled() {
  double v = 0;

  // div10 signed: raw 0xFFD8 (-40) * 0.1 = -4.0 C.
  const uint8_t neg[] = {0xD8, 0xFF};
  CHECK(decode_scaled(neg, 2, 2, /*signed*/ true, 0.1, &v));
  CHECK(close_to(v, -4.0));

  // div10 signed positive: raw 200 * 0.1 = 20.0 C (log-confirmed Aussen/Kessel).
  const uint8_t pos[] = {0xC8, 0x00};
  CHECK(decode_scaled(pos, 2, 2, true, 0.1, &v));
  CHECK(close_to(v, 20.0));

  // sec2hour on a 4-byte counter: 212,197,680 / 3600 = 58943.8 h. The raw
  // value exceeds float32's exact range; decoding in double keeps it accurate.
  const uint8_t secs[] = {0x30, 0xE1, 0xA5, 0x0C};
  CHECK(decode_scaled(secs, 4, 4, false, 1.0 / 3600.0, &v));
  CHECK(close_to(v, 58943.8, 0.05));

  // noconv len 1: raw byte, unsigned.
  const uint8_t one[] = {0x4B};
  CHECK(decode_scaled(one, 1, 1, false, 1.0, &v));
  CHECK(close_to(v, 75.0));

  // Short payload -> false, *out untouched.
  v = 123.0;
  const uint8_t shortp[] = {0x01};
  CHECK(!decode_scaled(shortp, 1, 2, false, 1.0, &v));
  CHECK(close_to(v, 123.0));
}

// --- encode_scaled (the write path; inverse of decode_scaled) --------------
static void test_encode_scaled() {
  uint8_t buf[4] = {0, 0, 0, 0};

  // Warmwassersolltemperatur: 50 C, scale 1.0, unsigned, len 1 -> 0x32.
  CHECK(encode_scaled(50.0, 1.0, false, 1, buf));
  CHECK(buf[0] == 0x32);

  // div10 slope: 3.5 -> raw 35 -> 0x23.
  CHECK(encode_scaled(3.5, 0.1, false, 1, buf));
  CHECK(buf[0] == 0x23);
  // 0.2 -> raw 2.
  CHECK(encode_scaled(0.2, 0.1, false, 1, buf));
  CHECK(buf[0] == 0x02);

  // Signed niveau: -13, scale 1.0, signed, len 1 -> 0xF3 (two's complement).
  CHECK(encode_scaled(-13.0, 1.0, true, 1, buf));
  CHECK(buf[0] == 0xF3);
  CHECK(encode_scaled(40.0, 1.0, true, 1, buf));
  CHECK(buf[0] == 0x28);

  // Range checks: unsigned len 1 max is 255.
  CHECK(!encode_scaled(300.0, 1.0, false, 1, buf));  // 300 > 255
  CHECK(!encode_scaled(-1.0, 1.0, false, 1, buf));   // negative, unsigned
  CHECK(!encode_scaled(128.0, 1.0, true, 1, buf));   // > int8 max 127
  CHECK(!encode_scaled(-129.0, 1.0, true, 1, buf));  // < int8 min -128

  // Round-trip: encode then decode returns the original (within rounding).
  CHECK(encode_scaled(21.0, 0.1, false, 1, buf));  // raw 210
  double back = 0;
  CHECK(decode_scaled(buf, 1, 1, false, 0.1, &back));
  CHECK(close_to(back, 21.0));

  // Non-finite and zero-scale are rejected.
  CHECK(!encode_scaled(std::nan(""), 1.0, false, 1, buf));
  CHECK(!encode_scaled(1.0, 0.0, false, 1, buf));

  // 2-byte unsigned boundary.
  CHECK(encode_scaled(65535.0, 1.0, false, 2, buf));
  CHECK(buf[0] == 0xFF && buf[1] == 0xFF);
  CHECK(!encode_scaled(65536.0, 1.0, false, 2, buf));
}

// --- bcd_to_int ------------------------------------------------------------
static void test_bcd() {
  uint8_t out = 0;
  CHECK(bcd_to_int(0x25, &out) && out == 25);
  CHECK(bcd_to_int(0x09, &out) && out == 9);
  CHECK(bcd_to_int(0x00, &out) && out == 0);
  // Non-BCD nibbles fail (0xFF is the empty-slot fill).
  CHECK(!bcd_to_int(0xFF, &out));
  CHECK(!bcd_to_int(0x9A, &out));
  CHECK(!bcd_to_int(0xA0, &out));
}

// --- decode_datetime_bcd ---------------------------------------------------
static void test_datetime() {
  BcdDateTime dt{};

  // Slot: code byte then DateTimeBCD. 2026-03-15 14:30:45, weekday=2 (ignored).
  // [0]=code [1]=year-hi(0x20) [2]=year-lo(0x26) [3]=month(0x03)
  // [4]=day(0x15) [5]=weekday(0x02) [6]=hour(0x14) [7]=min(0x30) [8]=sec(0x45)
  const uint8_t slot[] = {0xD1, 0x20, 0x26, 0x03, 0x15, 0x02, 0x14, 0x30, 0x45};
  CHECK(decode_datetime_bcd(slot, sizeof(slot), 1, &dt));
  CHECK(dt.year == 2026);
  CHECK(dt.month == 3);
  CHECK(dt.day == 15);
  CHECK(dt.hour == 14);
  CHECK(dt.minute == 30);
  CHECK(dt.second == 45);

  // Empty slot (0xFF-filled after the code) -> false.
  const uint8_t empty[] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(!decode_datetime_bcd(empty, sizeof(empty), 1, &dt));

  // All-zero -> year 0, rejected by the >= 1990 plausibility guard.
  const uint8_t zero[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  CHECK(!decode_datetime_bcd(zero, sizeof(zero), 1, &dt));

  // Valid BCD but impossible month (0x13 = 13) -> false.
  const uint8_t badmon[] = {0x00, 0x20, 0x26, 0x13, 0x15, 0x02, 0x14, 0x30, 0x45};
  CHECK(!decode_datetime_bcd(badmon, sizeof(badmon), 1, &dt));

  // Too short for offset+8 -> false.
  CHECK(!decode_datetime_bcd(slot, 5, 1, &dt));
}

// --- decode_masked_bit (Stage 1, unchanged) --------------------------------
static void test_masked_bit() {
  bool out = false;
  const uint8_t d[] = {0x00, 0x01, 0x80};
  CHECK(decode_masked_bit(d, 3, 1, 0x01, &out) && out == true);
  CHECK(decode_masked_bit(d, 3, 0, 0xFF, &out) && out == false);
  CHECK(decode_masked_bit(d, 3, 2, 0x80, &out) && out == true);
  CHECK(decode_masked_bit(d, 3, 2, 0x01, &out) && out == false);
  // Out-of-range offset -> false, *out untouched.
  out = true;
  CHECK(!decode_masked_bit(d, 3, 5, 0x01, &out));
  CHECK(out == true);
}

// --- decode_ascii (HexByte2AsciiByte) --------------------------------------
static void test_ascii() {
  char buf[24];
  // "7426" as ASCII codes 0x37 0x34 0x32 0x36.
  const uint8_t s1[] = {0x37, 0x34, 0x32, 0x36};
  CHECK(decode_ascii(s1, 4, 4, buf, sizeof(buf)) == 4);
  CHECK(std::strcmp(buf, "7426") == 0);
  // NUL terminates: "AB\0CD" -> "AB".
  const uint8_t s2[] = {0x41, 0x42, 0x00, 0x43, 0x44};
  CHECK(decode_ascii(s2, 5, 5, buf, sizeof(buf)) == 2);
  CHECK(std::strcmp(buf, "AB") == 0);
  // Trailing spaces trimmed: "7E211   " -> "7E211".
  const uint8_t s3[] = {0x37, 0x45, 0x32, 0x31, 0x31, 0x20, 0x20, 0x20};
  CHECK(decode_ascii(s3, 8, 8, buf, sizeof(buf)) == 5);
  CHECK(std::strcmp(buf, "7E211") == 0);
  // Non-printable byte -> '?'.
  const uint8_t s4[] = {0x41, 0xFF, 0x42};
  CHECK(decode_ascii(s4, 3, 3, buf, sizeof(buf)) == 3);
  CHECK(std::strcmp(buf, "A?B") == 0);
  // Bad args: payload shorter than len, and out_cap too small.
  CHECK(decode_ascii(s1, 2, 4, buf, sizeof(buf)) == -1);
  CHECK(decode_ascii(s1, 4, 4, buf, 2) == -1);
}

int main() {
  test_read_le();
  test_sign_extend();
  test_decode_scaled();
  test_encode_scaled();
  test_bcd();
  test_datetime();
  test_masked_bit();
  test_ascii();

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
