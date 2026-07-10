// Proof: string fields live INSIDE a block and must be sliced, not addressed.
//
// Beschriftung_HK1~0x7360 is BlockLength 42, BytePosition 2, ByteLength 40. The
// catalog generator used to emit `address: 0x7360 + 2 = 0x7362, length: 40`.
// 0x7362 is not a datapoint: P300 answers an unaligned interior read with an
// error telegram at ANY width (hardware, 2026-07-10: a 2-byte read at 0x7362
// and a 40-byte read at 0x7362 both returned MessageIdentifier 0x03), and KW
// answers it with 0xFF fill.
//
// The correct shape is an aligned block read at the base plus byte_offset. This
// proof pins the arithmetic VitoTextSensor::slice_() performs, against the
// decode helpers it feeds.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "decode.h"

using namespace esphome::vitohome;

static int failures = 0;

static void check(const char* what, bool cond) {
  printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
  if (!cond) failures++;
}

// Mirrors VitoTextSensor::slice_(): resolve (data, len) + byte_offset/byte_length
// to the field span. Returns false when the field does not fit the response.
static bool slice(const uint8_t* data, uint8_t len, int16_t extract_byte, uint8_t extract_len, const uint8_t*& field,
                  uint8_t& width) {
  field = data;
  width = len;
  if (extract_byte < 0) return true;
  const uint16_t off = static_cast<uint16_t>(extract_byte);
  if (off + extract_len > len) return false;
  field = data + off;
  width = extract_len;
  return true;
}

int main() {
  printf("proof_string_offset: aligned block read for ascii/utf16\n");

  // A realistic 42-byte label block: 2 leading bytes, then "Wohnzimmer" as
  // UTF-16LE, then 0xFFFF fill to byte 41.
  uint8_t block[42];
  std::memset(block, 0xFF, sizeof(block));
  block[0] = 0x2A;  // whatever the first two bytes are; NOT part of the label
  block[1] = 0x00;
  const char* label = "Wohnzimmer";
  for (std::size_t i = 0; i < std::strlen(label); i++) {
    block[2 + 2 * i] = static_cast<uint8_t>(label[i]);
    block[2 + 2 * i + 1] = 0x00;
  }

  char buf[80];
  const uint8_t* field = nullptr;
  uint8_t width = 0;

  // 1. The correct shape: block read of 42 at the base, field 40 bytes @ 2.
  bool ok = slice(block, 42, /*extract_byte=*/2, /*extract_len=*/40, field, width);
  check("slice(42, off 2, len 40) succeeds", ok);
  check("slice yields width 40", width == 40);
  check("slice yields base+2", field == block + 2);
  ok = ok && decode_utf16(field, width, width, buf, sizeof(buf)) >= 0;
  check("decodes to \"Wohnzimmer\"", ok && std::strcmp(buf, "Wohnzimmer") == 0);

  // 2. Reading the block base WITHOUT the offset silently eats the two leading
  //    bytes -- i.e. omitting byte_offset is not a benign shortcut.
  ok = slice(block, 42, /*extract_byte=*/-1, /*extract_len=*/1, field, width);
  check("slice without byte_offset returns the whole response", ok && width == 42 && field == block);
  const uint8_t clipped = width > 40 ? 40 : width;
  ok = decode_utf16(field, width, clipped, buf, sizeof(buf)) >= 0;
  check("no-offset decode does NOT yield the label", ok && std::strcmp(buf, "Wohnzimmer") != 0);

  // 3. Guard: a field that runs past the response is rejected, not read OOB.
  check("off 2 + len 40 > 22-byte response is rejected", !slice(block, 22, 2, 40, field, width));
  check("off 41 + len 40 > 42-byte response is rejected", !slice(block, 42, 41, 40, field, width));
  check("off 0 + len 42 fits a 42-byte response", slice(block, 42, 0, 42, field, width));

  // 4. A narrower, P300-proven read width (22 bytes) still yields a prefix of
  //    the label: byte_offset 2, byte_length 20 -> the first 10 code units.
  ok = slice(block, 22, 2, 20, field, width);
  check("slice(22, off 2, len 20) succeeds", ok && width == 20);
  ok = ok && decode_utf16(field, width, width, buf, sizeof(buf)) >= 0;
  check("22-byte block yields \"Wohnzimmer\" (10 chars)", ok && std::strcmp(buf, "Wohnzimmer") == 0);

  // 5. ASCII path: Sachnummer is BytePosition 0, so it must be untouched by the
  //    new code path.
  const uint8_t sach[7] = {'7', '2', '4', '8', '2', '6', '1'};
  ok = slice(sach, 7, -1, 1, field, width);
  check("ascii without byte_offset unchanged", ok && width == 7 && field == sach);
  ok = ok && decode_ascii(field, width, width, buf, sizeof(buf)) >= 0;
  check("ascii decodes \"7248261\"", ok && std::strcmp(buf, "7248261") == 0);

  // 6. KW's answer to the fabricated interior address was 40 x 0xFF. Decoding
  //    that yields an EMPTY string -- which is exactly what the old config
  //    published, and why the bug hid for so long.
  uint8_t fill[40];
  std::memset(fill, 0xFF, sizeof(fill));
  ok = decode_utf16(fill, 40, 40, buf, sizeof(buf)) >= 0;
  check("40 x 0xFF decodes to \"\" (the old silent failure)", ok && buf[0] == '\0');

  // 7. THE REAL WIRE BYTES. VScotHO1_72, P300, 2026-07-10, raw scan console:
  //      >>> 41:05:00:01:73:60:16:EF                    (read 0x7360, 22 bytes)
  //      <<< 06:41:1B:01:01:73:60:16:<22 bytes>:DF      (MessageIdentifier 0x01)
  //    Byte 0 = 0x00, byte 1 = 0x0B. Bytes 2..21 are the UTF-16LE label.
  //    The block base ANSWERS; only base+BytePosition is rejected.
  const uint8_t wire[22] = {0x00, 0x0B, 0x48, 0x00, 0x65, 0x00, 0x69, 0x00, 0x7A, 0x00, 0x6B,
                            0x00, 0x72, 0x00, 0x65, 0x00, 0x69, 0x00, 0x73, 0x00, 0x20, 0x00};
  ok = slice(wire, 22, 2, 20, field, width);
  check("wire: slice(22, off 2, len 20) succeeds", ok && width == 20);
  ok = ok && decode_utf16(field, width, width, buf, sizeof(buf)) >= 0;
  // decode_utf16 trims trailing spaces, so "Heizkreis " publishes as "Heizkreis".
  check("wire decodes to \"Heizkreis\"", ok && std::strcmp(buf, "Heizkreis") == 0);

  // 8. Decoding the same response WITHOUT byte_offset -- i.e. what an aligned
  //    read at the base would have published had we forgotten the offset --
  //    starts on 0x0B00 and is NOT the label.
  ok = decode_utf16(wire, 22, 22, buf, sizeof(buf)) >= 0;
  check("wire without byte_offset is not the label", ok && std::strcmp(buf, "Heizkreis") != 0);

  // 9. The 32-byte read of the same block, 2026-07-10:
  //      >>> 41:05:00:01:73:60:20:F9
  //      <<< 06:41:25:01:01:73:60:20:<32 bytes>:1C
  //    Byte 1 = 0x0B = 11 is the label's CHARACTER COUNT: "Heizkreis 1" is
  //    exactly 11 characters, and code units 11..14 are 0xFFFF fill, which
  //    decode_utf16() skips. This is the config the curated example ships
  //    (length 32, byte_offset 2, byte_length 30).
  const uint8_t wire32[32] = {0x00, 0x0B, 0x48, 0x00, 0x65, 0x00, 0x69, 0x00, 0x7A, 0x00, 0x6B,
                              0x00, 0x72, 0x00, 0x65, 0x00, 0x69, 0x00, 0x73, 0x00, 0x20, 0x00,
                              0x31, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  ok = slice(wire32, 32, 2, 30, field, width);
  check("wire32: slice(32, off 2, len 30) succeeds", ok && width == 30);
  ok = ok && decode_utf16(field, width, width, buf, sizeof(buf)) >= 0;
  check("wire32 decodes to \"Heizkreis 1\"", ok && std::strcmp(buf, "Heizkreis 1") == 0);
  check("byte 1 equals the decoded character count", wire32[1] == std::strlen(buf));

  // 10. The 22-byte read clips the same label one character short -- which is
  //     why the curated example widened to 32 once the wider read was proven.
  ok = slice(wire32, 22, 2, 20, field, width);
  check("wire32 clipped to 22 bytes still slices", ok && width == 20);
  ok = ok && decode_utf16(field, width, width, buf, sizeof(buf)) >= 0;
  check("22-byte read clips \"Heizkreis 1\" to \"Heizkreis\"", ok && std::strcmp(buf, "Heizkreis") == 0);

  // 11. The FULL 42-byte block, 2026-07-10:
  //       >>> 41:05:00:01:73:60:2A:03
  //       <<< 06:41:2F:01:01:73:60:2A:<42 bytes>:2B     (length byte 0x2F = 47)
  //     A 42-byte read SUCCEEDS on P300. It also shows the padding is a BYTE
  //     run, not code-unit aligned: thirteen 0xFF then five 0x00. Code unit 17
  //     is therefore 0x00FF, and the old "skip 0xFFFF" decode published
  //     "Heizkreis 1ÿ". decode_utf16() now terminates on 0xFFFF.
  const uint8_t wire42[42] = {0x00, 0x0B, 0x48, 0x00, 0x65, 0x00, 0x69, 0x00, 0x7A, 0x00, 0x6B, 0x00, 0x72, 0x00,
                              0x65, 0x00, 0x69, 0x00, 0x73, 0x00, 0x20, 0x00, 0x31, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
                              0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00};
  int ff = 0;
  for (int i = 2; i < 42; i++)
    if (wire42[i] == 0xFF) ff++;
  check("padding is an ODD byte run (13 x 0xFF), not code-unit aligned", ff == 13);
  ok = slice(wire42, 42, 2, 40, field, width);
  check("wire42: slice(42, off 2, len 40) succeeds", ok && width == 40);
  ok = ok && decode_utf16(field, width, width, buf, sizeof(buf)) >= 0;
  check("full field decodes to \"Heizkreis 1\" (no trailing 0x00FF)", ok && std::strcmp(buf, "Heizkreis 1") == 0);
  check("byte 1 still equals the character count", wire42[1] == std::strlen(buf));

  printf("%s\n", failures == 0 ? "proof_string_offset OK" : "proof_string_offset FAILED");
  return failures == 0 ? 0 : 1;
}
