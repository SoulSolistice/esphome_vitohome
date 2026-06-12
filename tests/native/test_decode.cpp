#include <cassert>
#include <cstdint>
#include <cstdio>

#include "../../components/vitohome/decode.h"

using esphome::vitohome::decode_masked_bit;

int main() {
  bool out = false;
  const uint8_t buf[] = {0x01, 0xF0, 0x00};

  // Bit set / clear in the first byte.
  assert(decode_masked_bit(buf, sizeof(buf), 0, 0x01, &out) && out);
  assert(decode_masked_bit(buf, sizeof(buf), 0, 0x02, &out) && !out);

  // Second byte: mask hits high nibble, misses low nibble.
  assert(decode_masked_bit(buf, sizeof(buf), 1, 0x80, &out) && out);
  assert(decode_masked_bit(buf, sizeof(buf), 1, 0x0F, &out) && !out);

  // Offset past the end and offset == length are both out of range.
  assert(!decode_masked_bit(buf, sizeof(buf), 5, 0xFF, &out));
  assert(!decode_masked_bit(buf, sizeof(buf), 3, 0x01, &out));

  // Null payload is rejected.
  assert(!decode_masked_bit(nullptr, 4, 0, 0x01, &out));

  std::puts("decode: all tests passed");
  return 0;
}
