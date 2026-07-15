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

#define CHECK(cond) \
  do { \
    ++g_checks; \
    if (!(cond)) { \
      ++g_failures; \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
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

  // llround-domain guard: magnitudes at/beyond 2^32 must be rejected BEFORE
  // std::llround runs. Out of range, llround's result is UNSPECIFIED per C17
  // (glibc saturates to LLONG_MAX/MIN -- host-verified -- but another libm
  // may return an in-range value and transmit garbage). Not UBSan-detectable
  // (a libm call, not a language-level cast), so these return-value
  // assertions are the regression guard.
  CHECK(!encode_scaled(1e30, 1.0, false, 4, buf));   // absurd value
  CHECK(!encode_scaled(1.0, 1e-30, false, 4, buf));  // absurd scale
  CHECK(!encode_scaled(-1e30, 1.0, true, 4, buf));   // absurd negative
  // The guard moves no accept/reject boundary: the widest legal raw
  // (2^32 - 1, unsigned len 4) still encodes, and 2^32 is still rejected by
  // range, exactly as before.
  CHECK(encode_scaled(4294967295.0, 1.0, false, 4, buf));
  CHECK(buf[0] == 0xFF && buf[1] == 0xFF && buf[2] == 0xFF && buf[3] == 0xFF);
  CHECK(!encode_scaled(4294967296.0, 1.0, false, 4, buf));
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

// --- format_raw_dump -------------------------------------------------------
static void test_format_raw_dump() {
  char buf[160];

  // 2-byte little-endian: raw 0xBBAA = 48042 unsigned, -17494 signed.
  const uint8_t two[] = {0xAA, 0xBB};
  format_raw_dump(0x0800, two, 2, buf, sizeof(buf));
  CHECK(std::strcmp(buf, "0x0800: AA BB  u=48042 i=-17494") == 0);

  // 1-byte signed view: 0xFF -> u=255 i=-1.
  const uint8_t one[] = {0xFF};
  format_raw_dump(0x55D3, one, 1, buf, sizeof(buf));
  CHECK(std::strcmp(buf, "0x55D3: FF  u=255 i=-1") == 0);

  // 4-byte counter past the float32-exact range (2^24): exact in u64.
  // LE bytes 80 0A A3 0C = 0x0CA30A80 = 212011648 (>> 2^24 = 16777216).
  const uint8_t four[] = {0x80, 0x0A, 0xA3, 0x0C};
  format_raw_dump(0x08A7, four, 4, buf, sizeof(buf));
  CHECK(std::strcmp(buf, "0x08A7: 80 0A A3 0C  u=212011648 i=212011648") == 0);

  // 9-byte payload (error-history slot): hex only, no integer view (len > 8).
  const uint8_t nine[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
  format_raw_dump(0x7507, nine, 9, buf, sizeof(buf));
  CHECK(std::strcmp(buf, "0x7507: 11 22 33 44 55 66 77 88 99") == 0);

  // Truncation is safe: tiny buffer is NUL-terminated and never overruns.
  char tiny[8];
  const int n = format_raw_dump(0x0800, two, 2, tiny, sizeof(tiny));
  CHECK(n == static_cast<int>(sizeof(tiny)) - 1);
  CHECK(tiny[sizeof(tiny) - 1] == '\0');
  CHECK(std::strncmp(tiny, "0x0800:", 7) == 0);

  // Bad args fail soft.
  CHECK(format_raw_dump(0x0800, two, 2, nullptr, sizeof(buf)) == 0);
  CHECK(format_raw_dump(0x0800, two, 2, buf, 0) == 0);
}

// --- int_to_bcd (system-time set) ------------------------------------------
static void test_int_to_bcd() {
  uint8_t b = 0;
  CHECK(int_to_bcd(25, &b) && b == 0x25);
  CHECK(int_to_bcd(0, &b) && b == 0x00);
  CHECK(int_to_bcd(9, &b) && b == 0x09);
  CHECK(int_to_bcd(99, &b) && b == 0x99);
  CHECK(int_to_bcd(20, &b) && b == 0x20);  // century byte
  CHECK(!int_to_bcd(100, &b));             // out of one-byte BCD range
  // Round-trips against the decoder.
  for (uint8_t v = 0; v <= 99; v++) {
    uint8_t enc = 0, dec = 0;
    CHECK(int_to_bcd(v, &enc));
    CHECK(bcd_to_int(enc, &dec) && dec == v);
  }
}

// --- timebyte_to_hhmm / hhmm_to_timebyte -----------------------------------
static void test_timebyte() {
  uint8_t h = 0, m = 0, b = 0;
  // 06:00 = (6<<3)|0 = 0x30 ; 22:00 = (22<<3)|0 = 0xB0.
  CHECK(timebyte_to_hhmm(0x30, &h, &m) && h == 6 && m == 0);
  CHECK(timebyte_to_hhmm(0xB0, &h, &m) && h == 22 && m == 0);
  // 08:30 = (8<<3)|3 = 0x43 (3*10 = 30 min).
  CHECK(timebyte_to_hhmm(0x43, &h, &m) && h == 8 && m == 30);
  // Disabled sentinel (0xFF -> hour 31) and any hour >= 24 -> false.
  CHECK(!timebyte_to_hhmm(0xFF, &h, &m));
  CHECK(!timebyte_to_hhmm(0xC0, &h, &m));  // hour 24

  // Encode truncates the minute to the next-lower 10-minute step.
  CHECK(hhmm_to_timebyte(6, 0, &b) && b == 0x30);
  CHECK(hhmm_to_timebyte(6, 7, &b) && b == 0x30);   // 06:07 -> 06:00
  CHECK(hhmm_to_timebyte(6, 9, &b) && b == 0x30);   // 06:09 -> 06:00
  CHECK(hhmm_to_timebyte(8, 30, &b) && b == 0x43);  // exact
  CHECK(hhmm_to_timebyte(8, 35, &b) && b == 0x43);  // 08:35 -> 08:30
  CHECK(hhmm_to_timebyte(23, 50, &b) && b == 0xBD);
  CHECK(hhmm_to_timebyte(23, 55, &b) && b == 0xBD);  // 23:55 -> 23:50, never 24:00
  // Hour 24+ would collide with the disabled sentinel -> rejected.
  CHECK(!hhmm_to_timebyte(24, 0, &b));
  CHECK(!hhmm_to_timebyte(6, 60, &b));  // minute out of range
}

// --- decode_schaltzeiten_day / encode_schaltzeiten_day ---------------------
static void test_schaltzeiten_day() {
  char out[64];

  // One window 06:00-22:00, remaining pairs disabled (0xFF) -> trailing
  // disabled pairs trimmed.
  const uint8_t one[] = {0x30, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(decode_schaltzeiten_day(one, sizeof(one), out, sizeof(out)) == 11);
  CHECK(std::strcmp(out, "06:00-22:00") == 0);

  // Two windows.
  const uint8_t two[] = {0x30, 0xB0, 0x43, 0x60, 0xFF, 0xFF, 0xFF, 0xFF};
  decode_schaltzeiten_day(two, sizeof(two), out, sizeof(out));
  CHECK(std::strcmp(out, "06:00-22:00 08:30-12:00") == 0);

  // Interior disabled pair keeps its slot as "--" (position preserved).
  const uint8_t gap[] = {0x30, 0xB0, 0xFF, 0xFF, 0x43, 0x60, 0xFF, 0xFF};
  decode_schaltzeiten_day(gap, sizeof(gap), out, sizeof(out));
  CHECK(std::strcmp(out, "06:00-22:00 -- 08:30-12:00") == 0);

  // All disabled -> empty string.
  const uint8_t none[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(decode_schaltzeiten_day(none, sizeof(none), out, sizeof(out)) == 0);
  CHECK(out[0] == '\0');

  // Short payload / bad args fail soft.
  CHECK(decode_schaltzeiten_day(one, 7, out, sizeof(out)) == -1);
  CHECK(decode_schaltzeiten_day(nullptr, 8, out, sizeof(out)) == -1);

  // Encode mirrors decode.
  uint8_t buf[8];
  CHECK(encode_schaltzeiten_day("06:00-22:00", buf));
  CHECK(std::memcmp(buf, one, 8) == 0);

  CHECK(encode_schaltzeiten_day("06:00-22:00 08:30-12:00", buf));
  CHECK(std::memcmp(buf, two, 8) == 0);

  CHECK(encode_schaltzeiten_day("06:00-22:00 -- 08:30-12:00", buf));
  CHECK(std::memcmp(buf, gap, 8) == 0);

  // Empty / blank clears the whole day (all 0xFF).
  CHECK(encode_schaltzeiten_day("", buf));
  CHECK(std::memcmp(buf, none, 8) == 0);
  CHECK(encode_schaltzeiten_day("   ", buf));
  CHECK(std::memcmp(buf, none, 8) == 0);
  CHECK(encode_schaltzeiten_day(nullptr, buf));
  CHECK(std::memcmp(buf, none, 8) == 0);

  // Minute truncation on the encode side.
  CHECK(encode_schaltzeiten_day("06:07-22:55", buf));
  CHECK(buf[0] == 0x30 && buf[1] == 0xB5);  // 06:00 and 22:50

  // Single-digit hour accepted.
  CHECK(encode_schaltzeiten_day("6:00-9:30", buf));
  CHECK(buf[0] == 0x30 && buf[1] == 0x4B);

  // Round-trip: decode(encode(canonical)) == canonical for a mixed day.
  CHECK(encode_schaltzeiten_day("06:00-22:00 -- 08:30-12:00", buf));
  decode_schaltzeiten_day(buf, 8, out, sizeof(out));
  CHECK(std::strcmp(out, "06:00-22:00 -- 08:30-12:00") == 0);

  // Malformed input is rejected; buf is left unchanged (no torn write). Seed
  // buf to a known value first so "unchanged" is what we actually assert.
  CHECK(encode_schaltzeiten_day("", buf));        // buf := all 0xFF
  CHECK(!encode_schaltzeiten_day("06:00", buf));  // missing OFF
  CHECK(std::memcmp(buf, none, 8) == 0);
  CHECK(!encode_schaltzeiten_day("24:00-01:00", buf));        // hour out of range
  CHECK(!encode_schaltzeiten_day("06:00-22:00-23:00", buf));  // trailing garbage
  CHECK(!encode_schaltzeiten_day("aa:bb-cc:dd", buf));        // non-numeric
  CHECK(!encode_schaltzeiten_day("1-2-3-4-5", buf));          // five tokens
}

// --- clock helpers: civil_seconds / weekday / encode_datetime_bcd ----------
static void test_clock_helpers() {
  // civil_seconds matches a Unix epoch computed as plain wall-clock (no tz).
  BcdDateTime dt{};
  dt.year = 2026;
  dt.month = 6;
  dt.day = 28;
  dt.hour = 14;
  dt.minute = 30;
  dt.second = 45;
  CHECK(civil_seconds(dt) == 1782657045LL);

  // Weekday map: ESPHome sunday=1..saturday=7 -> device sunday=0..saturday=6
  // (strftime %w). Wednesday=3 is hardware-confirmed on 0x20CB.
  CHECK(device_weekday_from_esptime(1) == 0);  // Sunday
  CHECK(device_weekday_from_esptime(2) == 1);  // Monday
  CHECK(device_weekday_from_esptime(3) == 2);  // Tuesday
  CHECK(device_weekday_from_esptime(4) == 3);  // Wednesday
  CHECK(device_weekday_from_esptime(7) == 6);  // Saturday

  // Encode the 8-byte DateTimeBCD wire layout. 2026-06-28 14:30:45 is a Sunday
  // (weekday byte = 0 in the sunday=0 convention).
  uint8_t buf[8];
  CHECK(encode_datetime_bcd(2026, 6, 28, 0, 14, 30, 45, buf));
  const uint8_t want[] = {0x20, 0x26, 0x06, 0x28, 0x00, 0x14, 0x30, 0x45};
  CHECK(std::memcmp(buf, want, 8) == 0);

  // Round-trip against the decoder (which ignores the weekday byte).
  BcdDateTime back{};
  CHECK(decode_datetime_bcd(buf, 8, 0, &back));
  CHECK(back.year == 2026 && back.month == 6 && back.day == 28);
  CHECK(back.hour == 14 && back.minute == 30 && back.second == 45);

  // Out-of-range fields are rejected and leave buf untouched.
  uint8_t seed[8];
  std::memcpy(seed, want, 8);
  std::memcpy(buf, want, 8);
  CHECK(!encode_datetime_bcd(2026, 13, 28, 0, 14, 30, 45, buf));  // month 13
  CHECK(std::memcmp(buf, seed, 8) == 0);
  CHECK(!encode_datetime_bcd(2026, 6, 28, 7, 14, 30, 45, buf));  // weekday 7
  CHECK(!encode_datetime_bcd(2026, 6, 28, 0, 24, 30, 45, buf));  // hour 24
}

// --- RotateBytes: read_be / decode_scaled_be ------------------------------
static void test_rotate_bytes() {
  // Big-endian assembly is MSB-first: {0x12,0x34} -> 0x1234 (vs read_le 0x3412).
  const uint8_t two[] = {0x12, 0x34};
  CHECK(read_be(two, 2) == 0x1234);
  CHECK(read_le(two, 2) == 0x3412);  // sanity: the two differ
  const uint8_t one[] = {0x96};
  CHECK(read_be(one, 1) == 0x96);  // single byte identical to LE

  double v = 0;
  // GWG_Codierstecker_Kennziffer-style 2-byte unsigned coding number.
  CHECK(decode_scaled_be(two, 2, 2, false, 1.0, &v) && close_to(v, 4660.0));
  // Signed BE: {0xFF,0xD8} = 0xFFD8 = -40 as int16, scaled 0.1 -> -4.0.
  const uint8_t neg[] = {0xFF, 0xD8};
  CHECK(decode_scaled_be(neg, 2, 2, true, 0.1, &v) && close_to(v, -4.0));
  // Short payload rejected.
  CHECK(!decode_scaled_be(two, 1, 2, false, 1.0, &v));
}

// --- HexByte2UTF16Byte: decode_utf16 --------------------------------------
static void test_utf16() {
  char buf[64];
  // "Wohnzimmer" as UTF-16LE (ASCII chars, low byte then 0x00).
  const uint8_t s1[] = {0x57, 0, 0x6F, 0, 0x68, 0, 0x6E, 0, 0x7A, 0, 0x69, 0, 0x6D, 0, 0x6D, 0, 0x65, 0, 0x72, 0};
  CHECK(decode_utf16(s1, sizeof(s1), sizeof(s1), buf, sizeof(buf)) == 10);
  CHECK(std::strcmp(buf, "Wohnzimmer") == 0);

  // 0x0000 terminates; trailing garbage ignored.
  const uint8_t s2[] = {0x41, 0, 0x42, 0, 0x00, 0, 0x5A, 0};
  CHECK(decode_utf16(s2, sizeof(s2), sizeof(s2), buf, sizeof(buf)) == 2);
  CHECK(std::strcmp(buf, "AB") == 0);

  // 0xFFFF empty-slot fill TERMINATES; trailing space trimmed.
  const uint8_t s3[] = {0x48, 0, 0x4B, 0, 0x31, 0, 0x20, 0, 0xFF, 0xFF};
  CHECK(decode_utf16(s3, sizeof(s3), sizeof(s3), buf, sizeof(buf)) == 3);
  CHECK(std::strcmp(buf, "HK1") == 0);

  // Regression: the device pads with a BYTE run of 0xFF that need not be code-
  // unit aligned. Thirteen 0xFF then five 0x00 makes code unit 17 = 0x00FF.
  // Skipping fill instead of terminating on it published "HK1ÿ".
  // (VScotHO1_72, P300, 2026-07-10, full 42-byte read of 0x7360.)
  const uint8_t s3b[] = {0x48, 0, 0x4B, 0, 0x31, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00};
  CHECK(decode_utf16(s3b, sizeof(s3b), sizeof(s3b), buf, sizeof(buf)) == 3);
  CHECK(std::strcmp(buf, "HK1") == 0);

  // Latin-1: 'ü' U+00FC -> UTF-8 0xC3 0xBC.
  const uint8_t s4[] = {0x42, 0, 0xFC, 0};
  CHECK(decode_utf16(s4, sizeof(s4), sizeof(s4), buf, sizeof(buf)) == 3);
  CHECK((unsigned char) buf[0] == 'B' && (unsigned char) buf[1] == 0xC3 && (unsigned char) buf[2] == 0xBC);

  // Odd byte length and short payload rejected.
  CHECK(decode_utf16(s1, sizeof(s1), 5, buf, sizeof(buf)) == -1);
  CHECK(decode_utf16(s1, 2, 4, buf, sizeof(buf)) == -1);
}

// --- Schaltzeiten interop with philippoo66 optolink-splitter ---------------
// Cross-checks the per-switch-point byte format against schedvdens / byte_to_hhmm
// in optolink-splitter/utils.py (hh = b>>3, mm = (b&7)*10, 0xFF = unused), so a
// program written here is byte-identical to what that project reads.
static void test_schaltzeiten_interop() {
  // byte_to_hhmm reference vectors (value -> "HH:MM").
  struct {
    uint8_t b;
    uint8_t h, m;
  } vec[] = {
      {0x00, 0, 0},    // 00:00
      {0x31, 6, 10},   // 49 -> 06:10
      {0x2D, 5, 50},   // 45 -> 05:50
      {0xB0, 22, 0},   // 176 -> 22:00
      {0xBD, 23, 50},  // 189 -> 23:50  (max valid; minute step 50 = low-3-bits 5)
  };
  for (auto &t : vec) {
    uint8_t h = 0, m = 0;
    timebyte_to_hhmm(t.b, &h, &m);
    CHECK(h == t.h && m == t.m);
    uint8_t enc = 0;
    CHECK(hhmm_to_timebyte(t.h, t.m, &enc) && enc == t.b);  // round-trip
  }
  CHECK(!timebyte_active(0xFF));  // 0xFF is the unused sentinel

  // A full day {ON=06:10, OFF=22:00} renders as schedvdens would print it.
  char out[48];
  const uint8_t day[] = {0x31, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(decode_schaltzeiten_day(day, sizeof(day), out, sizeof(out)) == 11);
  CHECK(std::strcmp(out, "06:10-22:00") == 0);
}

int main() {
  test_read_le();
  test_sign_extend();
  test_decode_scaled();
  test_encode_scaled();
  test_bcd();
  test_int_to_bcd();
  test_timebyte();
  test_schaltzeiten_day();
  test_clock_helpers();
  test_datetime();
  test_masked_bit();
  test_ascii();
  test_rotate_bytes();
  test_utf16();
  test_schaltzeiten_interop();
  test_format_raw_dump();

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
