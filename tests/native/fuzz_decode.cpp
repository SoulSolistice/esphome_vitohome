// Fuzz target + deterministic replay runner for the buffer-writing helpers in
// decode.h.
//
// WHY THESE FUNCTIONS. decode.h is where raw Optolink bytes become an ESPHome
// state, and the string half of it is the part that writes into a
// caller-supplied fixed buffer using hand-rolled offset arithmetic and
// snprintf truncation clamps -- `char out[64]` in vito_text.cpp, `char
// buf[160]` and `char buf[80]` in vito_text_sensor.cpp, `char buffer[208]` in
// vitohome.cpp. Every one of those caps is a constant a human worked out. The
// ParserVS2 target covers wire->payload; this covers payload->text, which is
// the other half of the path and the half with the pointer arithmetic.
//
// It also covers the one input in this codebase that originates with a PERSON
// rather than with the device: encode_schaltzeiten_day() parses an arbitrary
// Home Assistant string with raw `const char *` walking (parse_hhmm_), and a
// user can type anything into that field.
//
// Same two-build shape as fuzz_parser_vs2.cpp -- replay under g++ for CI,
// libFuzzer under clang for campaigns, one source so the oracles cannot drift.
// See that file's header and tests/native/README.md for the rationale.
//
// HOW THE BUFFER DISCIPLINE IS CHECKED. Every output buffer is heap-allocated
// at EXACTLY the capacity passed to the function, so a one-past-the-end write
// lands in an ASan redzone rather than in slack the test happened to reserve.
// The capacity itself comes out of the fuzz input, so tiny caps -- where the
// truncation branches live and where an off-by-one actually bites -- are
// explored rather than assumed away. Buffers are pre-filled with a non-zero
// canary so the NUL-termination assertions cannot pass vacuously against
// zeroed fresh heap.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "fuzz_corpus_decode.h"

// Relative include, matching test_decode.cpp: decode.h is framework-free and
// needs no include path.
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

constexpr char kCanary = '\xAA';

// Shared post-conditions for the "decode into a char buffer" family.
//
//   ret >= 0  : ret characters were written, out[ret] is the NUL, and the NUL
//               lies inside the caller's capacity. strnlen agreeing with ret
//               is what catches an embedded NUL or a miscounted offset -- a
//               truncated string that still reports the untruncated length.
//   nul_always: the function documents that it NUL-terminates whenever
//               cap > 0, including on its failure return. Only
//               decode_schaltzeiten_day (which clears out[0] before it
//               validates anything) and format_raw_dump promise that;
//               decode_ascii and decode_utf16 return -1 without touching the
//               buffer, so nothing may be asserted about its contents there.
void check_text_out(const char *out, std::size_t cap, int ret, bool nul_always, const char *what) {
  if (ret >= 0) {
    const std::size_t n = static_cast<std::size_t>(ret);
    oracle(n < cap, what);
    if (n < cap) {
      oracle(out[n] == '\0', what);
      // memchr rather than strnlen: strnlen is POSIX, not std, and this is
      // the same question -- is the FIRST NUL at index ret? An earlier NUL
      // means the reported length outran the string, which is how a truncated
      // value would still publish its untruncated size.
      const char *first_nul = static_cast<const char *>(std::memchr(out, '\0', cap));
      oracle(first_nul != nullptr && static_cast<std::size_t>(first_nul - out) == n, what);
    }
  }
  if (nul_always)
    oracle(std::memchr(out, '\0', cap) != nullptr, what);
}

}  // namespace

// One fuzz input drives every target. The first two bytes steer the shapes
// that matter -- the output capacity and the declared field width -- and the
// rest is the payload. Deriving the width independently of the real payload
// length is deliberate: `len` arrives from YAML on a real device, so a length
// that disagrees with the bytes actually read is a configuration the component
// must reject rather than a state it can assume away.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *input, std::size_t size) {
  const uint8_t cap_sel = size > 0 ? input[0] : 0;
  const uint8_t len_sel = size > 1 ? input[1] : 0;
  const uint8_t *data = size > 2 ? input + 2 : input;
  const std::size_t data_len = size > 2 ? size - 2 : 0;

  // 1..256, spanning every buffer a real caller passes: `char out[64]`
  // (vito_text.cpp), `char buf[80]` and `char buf[160]` (vito_text_sensor.cpp)
  // and `char buffer[208]` (vitohome.cpp's scan console).
  //
  // An earlier version capped this at 96, which seemed generous against the
  // longest canonical Schaltzeiten string (47 + NUL) -- and silently made one
  // branch unreachable. format_raw_dump's " ...(N bytes)" elision only runs
  // when the hex run is still inside the buffer at that point, and 48 bytes of
  // hex need 7 + 144 characters before the elision is even considered. With a
  // 96-byte ceiling the buffer was always full first, so no input could reach
  // it. A harness whose parameter range is narrower than the caller's is not
  // testing the caller; gcov is what exposed the difference.
  const std::size_t cap = 1u + cap_sel;
  // Deliberately allowed to exceed data_len: that is the rejected-config case.
  const uint8_t len = static_cast<uint8_t>(len_sel % 64u);

  char *out = new char[cap];

  // --- decode_schaltzeiten_day: 8 device bytes -> canonical string ----------
  std::memset(out, kCanary, cap);
  int r = decode_schaltzeiten_day(data, data_len, out, cap);
  check_text_out(out, cap, r, /*nul_always=*/true, "decode_schaltzeiten_day");

  // --- decode_ascii --------------------------------------------------------
  std::memset(out, kCanary, cap);
  r = decode_ascii(data, data_len, len, out, cap);
  check_text_out(out, cap, r, /*nul_always=*/false, "decode_ascii");

  // --- decode_utf16 --------------------------------------------------------
  std::memset(out, kCanary, cap);
  r = decode_utf16(data, data_len, len, out, cap);
  check_text_out(out, cap, r, /*nul_always=*/false, "decode_utf16");

  // --- format_raw_dump: the scan console's line for an arbitrary response ---
  std::memset(out, kCanary, cap);
  const uint16_t addr = static_cast<uint16_t>((cap_sel << 8) | len_sel);
  r = format_raw_dump(addr, data_len ? data : nullptr, static_cast<uint8_t>(data_len > 255 ? 255 : data_len), out, cap);
  check_text_out(out, cap, r, /*nul_always=*/true, "format_raw_dump");

  delete[] out;

  // --- decode_datetime_bcd -------------------------------------------------
  // No output buffer, so the oracle is semantic: accepting a slot must mean
  // the fields are in range. A BCD nibble check that let through, say, month
  // 0x19 would surface as a nonsense timestamp on an error-history sensor
  // rather than as a crash.
  BcdDateTime dt{};
  if (decode_datetime_bcd(data, data_len, len_sel % 4u, &dt)) {
    oracle(dt.year >= 1990 && dt.month >= 1 && dt.month <= 12 && dt.day >= 1 && dt.day <= 31 && dt.hour <= 23 &&
               dt.minute <= 59 && dt.second <= 59,
           "decode_datetime_bcd accepted an out-of-range field");
  }

  // --- encode_schaltzeiten_day: a HUMAN-typed string -> 8 device bytes ------
  // The payload is treated as the text a user typed into the Home Assistant
  // field, NUL-terminated the way the C API expects.
  {
    char *str = new char[data_len + 1];
    if (data_len)
      std::memcpy(str, data, data_len);
    str[data_len] = '\0';

    uint8_t *prog = new uint8_t[8];
    std::memset(prog, 0x5A, 8);
    const bool ok = encode_schaltzeiten_day(str, prog);

    if (ok) {
      // Round-trip, which decode.h states as a contract: "Canonical string
      // (round-trippable -- decode and encode share it)". Bytes that came out
      // of the encoder must decode to a canonical string that re-encodes to
      // the SAME bytes. This is the oracle that catches a silent wrong value
      // rather than a crash -- a program written to the boiler that reads back
      // as a different program.
      //
      // Only this direction is asserted. Device bytes -> string -> bytes is
      // NOT a contract: a malformed OFF byte (disabled while its ON is active)
      // renders through the same formula so the raw state stays visible, and
      // that rendering is deliberately not re-encodable.
      char canon[64];
      const int dr = decode_schaltzeiten_day(prog, 8, canon, sizeof(canon));
      oracle(dr >= 0, "encoded program failed to decode");
      if (dr >= 0) {
        uint8_t again[8];
        std::memset(again, 0xA5, 8);
        oracle(encode_schaltzeiten_day(canon, again), "canonical string failed to re-encode");
        oracle(std::memcmp(prog, again, 8) == 0, "Schaltzeiten round-trip changed the program bytes");
      }
    } else {
      // A rejected string must leave the caller's buffer untouched, so nothing
      // half-parsed is ever transmitted -- the same contract encode_scaled has.
      bool untouched = true;
      for (int i = 0; i < 8; i++)
        untouched = untouched && prog[i] == 0x5A;
      oracle(untouched, "rejected Schaltzeiten string still wrote to the buffer");
    }
    delete[] prog;
    delete[] str;
  }

  // --- encode_scaled / encode_raw_le: exact-size staging buffers ------------
  // Both take a width and a caller buffer, which is the shape that overflows.
  // The buffers are sized exactly so ASan frames them.
  {
    const uint8_t width = static_cast<uint8_t>(1u + (len_sel % 4u));  // encode_scaled: len in [1,4]
    double value = 0.0;
    double scale = 1.0;
    if (data_len >= sizeof(double))
      std::memcpy(&value, data, sizeof(double));
    if (data_len >= 2 * sizeof(double))
      std::memcpy(&scale, data + sizeof(double), sizeof(double));

    uint8_t *buf = new uint8_t[width];
    if (encode_scaled(value, scale, (len_sel & 0x80) != 0, width, buf)) {
      // Idempotence: decoding what was just encoded and re-encoding it must
      // reproduce the same bytes. Encoding rounds to the nearest raw step, so
      // a value that survived one round is already on a step and must not move
      // again. A drift here would mean a setpoint that changes every time it
      // is written back.
      double back = 0.0;
      const bool dec = decode_scaled(buf, width, width, (len_sel & 0x80) != 0, scale, &back);
      oracle(dec, "encode_scaled output failed to decode");
      if (dec) {
        uint8_t *again = new uint8_t[width];
        if (encode_scaled(back, scale, (len_sel & 0x80) != 0, width, again))
          oracle(std::memcmp(buf, again, width) == 0, "encode_scaled is not idempotent over its own output");
        delete[] again;
      }
    }
    delete[] buf;

    // encode_raw_le CLAMPS len to cap rather than trusting it, so an
    // over-long write length cannot walk past the staging buffer. Hand it a
    // deliberately over-long len against an exact-size buffer.
    const uint8_t raw_cap = static_cast<uint8_t>(1u + (cap_sel % 4u));
    uint8_t *raw = new uint8_t[raw_cap];
    const uint8_t written = encode_raw_le(static_cast<uint32_t>(addr) * 65537u, len, raw, raw_cap);
    oracle(written <= raw_cap, "encode_raw_le reported more bytes than the buffer holds");
    delete[] raw;
  }

  return 0;
}

#ifndef VITOHOME_FUZZ_LIBFUZZER

namespace {

bool replay_file(const char *path) {
  std::FILE *f = std::fopen(path, "rb");
  if (f == nullptr) {
    std::fprintf(stderr, "  cannot open %s\n", path);
    return false;
  }
  static uint8_t buf[64 * 1024];
  const std::size_t n = std::fread(buf, 1, sizeof(buf), f);
  std::fclose(f);
  g_case = path;
  const int before = g_fail;
  LLVMFuzzerTestOneInput(buf, n);
  std::printf("  %-40s %4zu bytes  %s\n", path, n, g_fail == before ? "ok" : "FAIL");
  return g_fail == before;
}

}  // namespace

int main(int argc, char **argv) {
  std::printf("== decode.h fuzz corpus (deterministic replay) ==\n");

  for (std::size_t i = 0; i < kDecodeSeedCount; ++i) {
    const DecodeSeed &s = kDecodeSeeds[i];
    g_case = s.name;
    const int before = g_fail;
    LLVMFuzzerTestOneInput(s.bytes, s.len);
    std::printf("  %-40s %4zu bytes  %s\n", s.name, s.len, g_fail == before ? "ok" : "FAIL");
  }

  for (int a = 1; a < argc; ++a)
    replay_file(argv[a]);

  std::printf("decode.h fuzz corpus: %zu seed(s), %d failure(s)\n", kDecodeSeedCount, g_fail);
  return g_fail == 0 ? 0 : 1;
}

#endif  // VITOHOME_FUZZ_LIBFUZZER
