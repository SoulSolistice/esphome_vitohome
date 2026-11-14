// Fuzz target + deterministic replay runner for the value half of decode.h:
// how a read response becomes an entity's number, and how a number becomes the
// bytes a write sends. fuzz_decode.cpp covers the text half.
//
// COVERED. field_slice() and extract_field(), which every sensor, number,
// select, switch and enum text sensor reads its field through;
// decode_masked_bit(); decode_scaled() / decode_scaled_be() / sign_extend_le();
// the encode_scaled() inverse; the BCD and DateTimeBCD codecs of the clock
// and error history; the switch-point byte codec; utf16_label_bytes();
// setpoint_byte().
//
// INPUT LAYOUT
//
//   [0] have       response length, b % 72 (past MAX_PAYLOAD on purpose)
//   [1] offset     byte_offset as an int8_t (negative: from the start)
//   [2] width      b % 10
//   [3] mask       bit_mask
//   [4] steering   bit 7 signed; bits 0-2 pick the converter scale
//   [5..] payload  the response bytes; missing ones read as 0
//
// The response is copied into a buffer of exactly `have` bytes, so a read one
// past it is an ASan report rather than slack.
//
// ORACLES
//   (1) field_slice() accepts exactly the fields inside the response and
//       points at byte_offset (0 when negative);
//   (2) extract_field() accepts exactly those with a contiguous or zero mask,
//       and returns the little-endian value of the first four bytes or the
//       masked bits shifted down;
//   (3) a field merged into the block with merge_block_field() reads back
//       through extract_field() as the staged value;
//   (4) decode_masked_bit() answers for an in-range byte only, with that bit;
//   (5) decode_scaled_be() of the reversed bytes equals decode_scaled(), and
//       the signed raw value lies inside its width's range;
//   (6) a value decode_scaled() delivered encodes back to the same bytes;
//   (7) BCD bytes, DateTimeBCD blocks and switch-point bytes survive a decode
//       and encode round trip, and what one side refuses the other does too;
//   (8) utf16_label_bytes() never exceeds the field, and a count it derives
//       is even;
//   (9) setpoint_byte() stays inside [min, max], NaN included.
//
// Same two-build arrangement as fuzz_parser_vs2.cpp; seeds in
// fuzz_corpus_field.h.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "fuzz_corpus_field.h"

#include "../../components/vitohome/decode.h"

using namespace esphome::vitohome;

namespace {

#ifndef VITOHOME_FUZZ_LIBFUZZER
int g_fail = 0;
#endif
const char *g_case = "(fuzz)";

void oracle(bool ok, const char *what) {
  if (ok)
    return;
  std::fprintf(stderr, "ORACLE VIOLATED [%s]: %s\n", g_case, what);
#ifdef VITOHOME_FUZZ_LIBFUZZER
  std::abort();
#else
  ++g_fail;
#endif
}

// The converter scales the catalog uses, plus two that are not exact in binary.
constexpr double kScales[8] = {1.0, 0.1, 0.5, 0.01, 2.0, 10.0, 3600.0, 0.001};

volatile uint32_t g_sink = 0;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, std::size_t size) {
  if (size < 5)
    return 0;
  const std::size_t have = data[0] % 72u;
  const int byte_offset = static_cast<int8_t>(data[1]);
  const uint8_t width = static_cast<uint8_t>(data[2] % 10u);
  const uint8_t mask = data[3];
  const bool is_signed = (data[4] & 0x80) != 0;
  const double scale = kScales[data[4] & 0x07];

  // Exactly `have` bytes; new[0] still yields a distinct pointer ASan tracks.
  uint8_t *resp = new uint8_t[have == 0 ? 1 : have];
  for (std::size_t i = 0; i < have; ++i)
    resp[i] = (5 + i < size) ? data[5 + i] : 0;
  const std::size_t off = byte_offset >= 0 ? static_cast<std::size_t>(byte_offset) : 0;
  const bool fits = width != 0 && off + width <= have;

  // --- (1) field_slice ------------------------------------------------------
  {
    const uint8_t *field = nullptr;
    const bool ok = field_slice(resp, have, byte_offset, width, &field);
    oracle(ok == fits, "field_slice accepts a field outside the response or refuses one inside");
    if (ok) {
      oracle(field == resp + off, "field_slice points somewhere else");
      for (uint8_t i = 0; i < width; ++i)
        g_sink = g_sink + field[i];
    }
  }

  // --- (2) extract_field ----------------------------------------------------
  {
    uint32_t value = 0xDEADBEEF;
    const bool ok = extract_field(resp, have, byte_offset, width, mask, &value);
    oracle(ok == (fits && (mask == 0 || bit_mask_contiguous(mask))), "extract_field accept/refuse");
    if (ok && mask == 0) {
      oracle(value == static_cast<uint32_t>(read_le(resp + off, width > 4 ? 4 : width)),
             "extract_field: not the little-endian value");
    } else if (ok) {
      oracle(value <= bit_field_max(mask), "extract_field: wider than its mask");
      oracle(value == static_cast<uint32_t>((resp[off] & mask) >> bit_mask_shift(mask)),
             "extract_field: not the masked bits");
    }
  }

  // --- (3) merge, then read back through the entity path --------------------
  // The staged field is the payload's tail, little-endian as an entity stages
  // it; a masked one carries the field value in its first byte.
  if (byte_offset >= 0 && have > 0 && have <= 255 && width >= 1 && width <= 4) {
    uint8_t staged[4] = {0, 0, 0, 0};
    for (uint8_t i = 0; i < width; ++i)
      staged[i] = data[size - 1 - i];
    if (mask != 0 && bit_mask_contiguous(mask))
      staged[0] = static_cast<uint8_t>(staged[0] % (bit_field_max(mask) + 1u));
    uint8_t *out = new uint8_t[have];
    if (merge_block_field(resp, static_cast<uint8_t>(have), static_cast<uint8_t>(off), width, mask, staged, out)) {
      uint32_t back = 0;
      const bool ok = extract_field(out, have, byte_offset, width, mask, &back);
      oracle(ok, "a merged field does not extract");
      const uint32_t want = mask != 0 ? staged[0] : static_cast<uint32_t>(read_le(staged, width));
      oracle(!ok || back == want, "a merged field reads back as another value");
    }
    delete[] out;
  }

  // --- (4) decode_masked_bit ------------------------------------------------
  {
    const uint8_t boff = static_cast<uint8_t>(data[1]);
    bool bit = false;
    const bool ok = decode_masked_bit(resp, have, boff, mask, &bit);
    oracle(ok == (boff < have), "decode_masked_bit accept/refuse");
    if (ok)
      oracle(bit == ((resp[boff] & mask) != 0), "decode_masked_bit: wrong bit");
  }

  // --- (5) little- vs big-endian, sign extension -----------------------------
  const uint8_t len = static_cast<uint8_t>(width == 0 ? 1 : (width > 8 ? 8 : width));
  if (have >= len) {
    uint8_t *rev = new uint8_t[len];
    for (uint8_t i = 0; i < len; ++i)
      rev[i] = resp[len - 1 - i];
    double le = 0.0, be = 0.0;
    const bool ok_le = decode_scaled(resp, have, len, is_signed, scale, &le);
    const bool ok_be = decode_scaled_be(rev, len, len, is_signed, scale, &be);
    oracle(ok_le && ok_be, "decode_scaled refused a response long enough");
    oracle(le == be, "decode_scaled_be of the reversed bytes differs");
    const uint64_t raw = read_le(resp, len);
    const int64_t s = sign_extend_le(raw, len);
    if (len < 8) {
      const int64_t half = int64_t{1} << (8 * len - 1);
      oracle(s >= -half && s < half, "sign_extend_le outside its width's range");
      oracle((static_cast<uint64_t>(s) & ((uint64_t{1} << (8 * len)) - 1)) == raw,
             "sign_extend_le changed the low bytes");
    }
    delete[] rev;

    // --- (6) decode, encode, same bytes ----------------------------------------
    if (len <= 4 && ok_le) {
      uint8_t *enc = new uint8_t[len];
      const bool ok = encode_scaled(le, scale, is_signed, len, enc);
      oracle(ok, "encode_scaled refused a value decode_scaled delivered");
      oracle(!ok || std::memcmp(enc, resp, len) == 0, "decode/encode round trip changed the bytes");
      delete[] enc;
    }
  }

  // --- (7) BCD, DateTimeBCD, switch points ------------------------------------
  for (std::size_t i = 0; i < have; ++i) {
    const uint8_t b = resp[i];
    uint8_t dec = 0, back = 0;
    if (bcd_to_int(b, &dec)) {
      oracle(dec <= 99 && int_to_bcd(dec, &back) && back == b, "BCD byte does not round-trip");
    } else {
      oracle((b >> 4) > 9 || (b & 0x0F) > 9, "bcd_to_int refused a BCD byte");
    }
    const uint8_t d = static_cast<uint8_t>(b % 100u);
    oracle(int_to_bcd(d, &back) && bcd_to_int(back, &dec) && dec == d, "decimal does not round-trip through BCD");
    oracle(!int_to_bcd(static_cast<uint8_t>(100u + b % 156u), &back), "int_to_bcd took a value above 99");

    uint8_t h = 0, m = 0, tb = 0;
    if (timebyte_to_hhmm(b, &h, &m)) {
      const bool enc = hhmm_to_timebyte(h, m, &tb);
      if ((b & 0x07) <= 5) {
        oracle(enc && tb == b, "switch-point byte does not round-trip");
      } else {
        oracle(!enc, "a minute code above 5 encoded");
      }
    } else {
      oracle((b >> 3) >= 24, "timebyte_to_hhmm refused an hour below 24");
    }
  }
  if (have >= 8) {
    const std::size_t doff = (off + 8 <= have) ? off : have - 8;
    BcdDateTime dt{};
    if (decode_datetime_bcd(resp, have, doff, &dt)) {
      oracle(dt.year >= 1990 && dt.month >= 1 && dt.month <= 12 && dt.day >= 1 && dt.day <= 31 && dt.hour <= 23 &&
                 dt.minute <= 59 && dt.second <= 59,
             "decode_datetime_bcd delivered an out-of-range field");
      uint8_t wd = 0;
      const bool wd_ok = bcd_to_int(resp[doff + 4], &wd) && wd <= 7;
      uint8_t enc[8];
      const bool ok =
          encode_datetime_bcd(dt.year, dt.month, dt.day, wd_ok ? wd : 0, dt.hour, dt.minute, dt.second, enc);
      oracle(ok, "encode_datetime_bcd refused a decoded date");
      for (uint8_t i = 0; ok && i < 8; ++i) {
        if (i != 4 || wd_ok)
          oracle(enc[i] == resp[doff + i], "DateTimeBCD does not round-trip");
      }
    }
  }

  // --- (8) utf16_label_bytes ---------------------------------------------------
  {
    const uint8_t n = utf16_label_bytes(resp, have, off, width);
    oracle(n <= width, "utf16_label_bytes exceeds the field");
    if (n != width)
      oracle(n % 2 == 0, "utf16_label_bytes derived an odd count");
  }

  // --- (9) setpoint_byte ------------------------------------------------------
  {
    const int min_c = data[1] % 40;
    const int max_c = min_c + data[2] % 50;
    float target = static_cast<float>(static_cast<int16_t>(data[3] << 8 | data[4])) / 10.0f;
    if ((data[4] & 0x18) == 0x18)
      target = std::numeric_limits<float>::quiet_NaN();
    else if ((data[4] & 0x18) == 0x10)
      target = (data[3] & 1) ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
    const uint8_t sp = setpoint_byte(target, min_c, max_c);
    oracle(sp >= min_c && sp <= max_c, "setpoint_byte outside [min, max]");
    if (std::isnan(target))
      oracle(sp == min_c, "setpoint_byte of NaN is not min");
  }

  delete[] resp;
  return 0;
}

#ifndef VITOHOME_FUZZ_LIBFUZZER

int main(int argc, char **argv) {
  std::printf("== field fuzz corpus (deterministic replay) ==\n");
  for (std::size_t i = 0; i < kFieldSeedCount; ++i) {
    const FieldSeed &s = kFieldSeeds[i];
    g_case = s.name;
    const int before = g_fail;
    LLVMFuzzerTestOneInput(s.bytes, s.len);
    std::printf("  %-40s %4zu bytes  %s\n", s.name, s.len, g_fail == before ? "ok" : "FAIL");
  }
  for (int a = 1; a < argc; ++a) {
    std::FILE *f = std::fopen(argv[a], "rb");
    if (f == nullptr)
      continue;
    static uint8_t buf[64 * 1024];
    const std::size_t n = std::fread(buf, 1, sizeof(buf), f);
    std::fclose(f);
    g_case = argv[a];
    const int before = g_fail;
    LLVMFuzzerTestOneInput(buf, n);
    std::printf("  %-40s %4zu bytes  %s\n", argv[a], n, g_fail == before ? "ok" : "FAIL");
  }
  std::printf("field fuzz corpus: %zu seed(s), %d failure(s)\n", kFieldSeedCount, g_fail);
  return g_fail == 0 ? 0 : 1;
}

#endif  // VITOHOME_FUZZ_LIBFUZZER
