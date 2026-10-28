// Fuzz target + deterministic replay runner for parse_raw_spec() (raw_op.h).
//
// The spec is a line a person types into Home Assistant ("Scan · Raw op") and
// the hub parses on the device, so any byte sequence must be handled. Same
// two-build arrangement as fuzz_parser_vs2.cpp: the replay runner walks the
// committed seeds in fuzz_corpus_raw_spec.h on every CI run; fuzz.sh builds
// the libFuzzer target for the open-ended search.
//
// Oracles, besides the sanitizers:
//   (1) a refusal names its field: error is non-null and non-empty;
//   (2) an accepted spec is within every bound queue_raw_op() relies on --
//       a layer, a code of 1..1F (P300) or 1..FF, a GWG address of one byte,
//       1..RAW_OP_PAYLOAD_MAX payload bytes, a read length of at most the cap
//       and 0 exactly for a P300 call without parameter;
//   (3) it round-trips: printed in canonical form and parsed again, it gives
//       the same fields and payload. That is what catches a parser that
//       accepts a line and silently reads something else from it;
//   (4) a payload is never classed READ or PROBE (raw_op_class()).

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "fuzz_corpus_raw_spec.h"
#include "raw_op.h"

using namespace esphome::vitohome;

namespace {

#ifndef VITOHOME_FUZZ_LIBFUZZER
int g_fail = 0;
#endif
const char *g_case = "(fuzz)";
constexpr uint8_t kReadMax = 48;

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

bool same(const RawSpec &a, const RawSpec &b) {
  if (a.layer != b.layer || a.code != b.code || a.address != b.address || a.length != b.length ||
      a.has_payload != b.has_payload)
    return false;
  return !a.has_payload || std::memcmp(a.payload, b.payload, a.length) == 0;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, std::size_t size) {
  // The text as the hub gets it: NUL-terminated, cut at an embedded NUL.
  // Heap-allocated to its exact size so ASan sees a read past the end.
  char *text = static_cast<char *>(std::malloc(size + 1));
  std::memcpy(text, data, size);
  text[size] = '\0';

  RawSpec spec;
  const char *error = nullptr;
  const bool ok = parse_raw_spec(text, spec, error, kReadMax);
  if (!ok) {
    oracle(error != nullptr && error[0] != '\0', "a refusal without a message");
    std::free(text);
    return 0;
  }

  oracle(error == nullptr, "an accepted spec with an error message");
  oracle(spec.layer == RawLayer::P300 || spec.layer == RawLayer::KW || spec.layer == RawLayer::GWG,
         "accepted without a layer");
  oracle(spec.code != 0 && (spec.layer != RawLayer::P300 || spec.code <= 0x1F), "code out of range");
  oracle(spec.layer != RawLayer::GWG || spec.address <= 0xFF, "GWG address above FF");
  if (spec.has_payload) {
    oracle(spec.length >= 1 && spec.length <= RAW_OP_PAYLOAD_MAX, "payload length out of range");
  } else {
    const bool call = spec.layer == RawLayer::P300 && spec.code == 0x07;
    oracle(spec.length <= kReadMax, "read length above the cap");
    oracle((spec.length == 0) == call, "length 0 for something other than a call");
  }
  const RawClass cls = raw_op_class(spec.layer, spec.code, spec.has_payload);
  oracle(!spec.has_payload || (cls != RawClass::READ && cls != RawClass::PROBE), "a payload classed as a read");

  // (3) canonical form: "<layer> <r|w> <code> <address> <length | bytes>".
  char line[256];
  int n = std::snprintf(line, sizeof(line), "%s %s %02X %04X", raw_layer_name(spec.layer), spec.has_payload ? "w" : "r",
                        spec.code, spec.address);
  if (spec.has_payload) {
    for (uint8_t i = 0; i < spec.length && n > 0 && n < static_cast<int>(sizeof(line)); ++i)
      n += std::snprintf(line + n, sizeof(line) - n, " %02X", spec.payload[i]);
  } else {
    n += std::snprintf(line + n, sizeof(line) - n, " %u", spec.length);
  }
  RawSpec again;
  const char *error2 = nullptr;
  const bool ok2 = parse_raw_spec(line, again, error2, kReadMax);
  oracle(ok2 && same(spec, again), "the canonical form does not parse back to the same operation");

  std::free(text);
  return 0;
}

#ifndef VITOHOME_FUZZ_LIBFUZZER

int main(int argc, char **argv) {
  std::printf("== raw op spec fuzz corpus (deterministic replay) ==\n");
  for (std::size_t i = 0; i < kRawSpecSeedCount; ++i) {
    const RawSpecSeed &s = kRawSpecSeeds[i];
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
    const std::size_t len = std::fread(buf, 1, sizeof(buf), f);
    std::fclose(f);
    g_case = argv[a];
    LLVMFuzzerTestOneInput(buf, len);
  }
  std::printf("raw op spec fuzz corpus: %zu seed(s), %d failure(s)\n", kRawSpecSeedCount, g_fail);
  return g_fail == 0 ? 0 : 1;
}

#endif  // VITOHOME_FUZZ_LIBFUZZER
