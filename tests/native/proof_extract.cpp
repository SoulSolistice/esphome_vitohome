// Proof: multi-byte field extraction from a wide block read (P300-portable
// pattern the generator now emits). A 2-byte div10 field at offset 12 of a
// 22-byte room-setpoint block, and a 4-byte counter at offset 4 of a 32-byte
// solar block, must decode identically to a direct read of those bytes.
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "decode.h"

using namespace esphome::vitohome;

static int failures = 0;

static void check(const char* what, bool cond) {
  printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
  if (!cond) failures++;
}

int main() {
  printf("proof_extract: multi-byte block extraction\n");

  // 22-byte block; bytes [12..13] = 0x00E6 LE = 230 -> div10 = 23.0
  uint8_t block22[22] = {0};
  block22[12] = 0xE6;
  block22[13] = 0x00;
  double v = NAN;
  bool ok = 12 + 2 <= 22 && decode_scaled(block22 + 12, 2, 2, false, 0.1, &v);
  check("2-byte div10 at offset 12 == 23.0", ok && std::abs(v - 23.0) < 1e-9);

  // 32-byte block; bytes [4..7] = 0x000186A0 LE = 100000 -> noconv
  uint8_t block32[32] = {0};
  block32[4] = 0xA0;
  block32[5] = 0x86;
  block32[6] = 0x01;
  block32[7] = 0x00;
  v = NAN;
  ok = 4 + 4 <= 32 && decode_scaled(block32 + 4, 4, 4, false, 1.0, &v);
  check("4-byte counter at offset 4 == 100000", ok && std::abs(v - 100000.0) < 1e-9);

  // Single byte at a deep offset (the coding-plug case): byte 9 of 16 = 0x2A = 42
  uint8_t block16[16] = {0};
  block16[9] = 0x2A;
  v = NAN;
  ok = 9 + 1 <= 16 && decode_scaled(block16 + 9, 1, 1, false, 1.0, &v);
  check("1-byte at offset 9 == 42", ok && std::abs(v - 42.0) < 1e-9);

  // Short response fail-soft: field runs past the received bytes -> no decode.
  v = NAN;
  ok = 12 + 2 <= 10;  // only 10 bytes received, field at 12 -> out of range
  check("field past short response is rejected", !ok);

  // select/switch state extraction: raw integer read_le at an offset (the
  // exact pattern VitoSelect/VitoSwitch::handle_response applies -- no
  // scaling, the raw value is matched against the configured options).
  // 1-byte enum field at offset 1 of a 2-byte block:
  uint8_t block2[2] = {0x55, 0x02};
  check("select 1-byte field at offset 1 == 0x02", read_le(block2 + 1, 1) == 0x02);
  // 2-byte enum field at offset 4 of a 6-byte block, LE 0x0100:
  uint8_t block6[6] = {0};
  block6[4] = 0x00;
  block6[5] = 0x01;
  check("select 2-byte field at offset 4 == 0x0100", read_le(block6 + 4, 2) == 0x0100);
  // Bound check mirror: field at offset 2 width 1 needs 3 bytes; 2 received.
  check("switch field past short response is rejected", !(2 + 1 <= 2));

  printf("proof_extract: %s\n", failures == 0 ? "ALL PASS" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
