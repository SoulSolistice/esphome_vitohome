// Differential target: each engine fuzz harness runs once against the
// reference engines (reference/, a frozen copy of today's) and once against
// the live ones, on the same input and the same clock, and the two runs must
// agree pass by pass. This is the acceptance check of the engine redesign
// (design-optolink-layers.md, section 9): the same bytes at the same moments.
//
// INPUT LAYOUT
//
//   [0] harness   bits 0-6 % 4: 0 fuzz_kw_session.cpp, 1 fuzz_gwg_session.cpp,
//                 2 fuzz_raw_engines.cpp, 3 fuzz_sim_session.cpp; bit 7: the
//                 script's time steps are 1 ms instead of 10 ms (the
//                 simulator session has no script and ignores it)
//   then          that harness's input, as its corpus header describes it
//
// ORACLES:
//   (1) both sides run the same number of engine passes, write the same bytes
//       in each pass, and report the same callbacks with the same addresses
//       and data, in the same order;
//   (2) no harness oracle fails on either side.
//
// Each harness is compiled twice with -DVITOHOME_DIFF_SIDE (diff_hooks.h);
// see build_and_run_protocols.sh. The replay build walks the four corpora,
// each scripted seed in 10 ms and in 1 ms steps, so a timing difference under one
// step of the seeds still shows; the libFuzzer build (-DVITOHOME_FUZZ_LIBFUZZER) aborts on the first
// divergence.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "diff_hooks.h"
#include "fuzz_corpus_diff.h"
#include "fuzz_corpus_gwg_session.h"
#include "fuzz_corpus_kw_session.h"
#include "fuzz_corpus_raw_engines.h"
#include "fuzz_corpus_sim_session.h"
#include "optolink_clock.h"

extern "C" int diff_kw_session_live(const uint8_t *data, std::size_t size);
extern "C" int diff_kw_session_ref(const uint8_t *data, std::size_t size);
extern "C" int diff_gwg_session_live(const uint8_t *data, std::size_t size);
extern "C" int diff_gwg_session_ref(const uint8_t *data, std::size_t size);
extern "C" int diff_raw_engines_live(const uint8_t *data, std::size_t size);
extern "C" int diff_raw_engines_ref(const uint8_t *data, std::size_t size);
extern "C" int diff_sim_session_live(const uint8_t *data, std::size_t size);
extern "C" int diff_sim_session_ref(const uint8_t *data, std::size_t size);

namespace {

namespace internals = esphome::vitohome::optolink::internals;

using Entry = int (*)(const uint8_t *, std::size_t);
struct Harness {
  const char *name;
  Entry live;
  Entry ref;
};
constexpr Harness kHarnesses[4] = {
    {"kw session", diff_kw_session_live, diff_kw_session_ref},
    {"gwg session", diff_gwg_session_live, diff_gwg_session_ref},
    {"raw engines", diff_raw_engines_live, diff_raw_engines_ref},
    {"sim session", diff_sim_session_live, diff_sim_session_ref},
};

// Where both sides start their clock. Not 0: the engines' timers start at 0,
// so a start near 0 would hide a timer left unset.
constexpr uint32_t kClockStart = 1000000;

struct Record {
  bool pass;  // an engine pass, or else a callback
  DiffEvent kind;
  uint16_t address;
  bool has_data;
  std::size_t length;
  std::vector<uint8_t> bytes;  // a pass: what it wrote; a callback: its data
};

struct Trace {
  std::vector<Record> records;
  std::size_t written = 0;
  std::vector<std::string> oracles;
};

Trace *g_trace = nullptr;

std::string describe(const Record &r) {
  static const char *const kKinds[] = {"response", "error", "timing", "fallback", "late"};
  char head[64];
  if (r.pass) {
    std::snprintf(head, sizeof(head), "pass, wrote %zu byte(s)", r.bytes.size());
  } else {
    std::snprintf(head, sizeof(head), "%s 0x%04X, %zu byte(s)%s", kKinds[static_cast<int>(r.kind)], r.address, r.length,
                  r.has_data ? "" : " (no data)");
  }
  std::string out = head;
  for (std::size_t i = 0; i < r.bytes.size() && i < 24; ++i) {
    char hex[4];
    std::snprintf(hex, sizeof(hex), " %02X", r.bytes[i]);
    out += hex;
  }
  if (r.bytes.size() > 24)
    out += " ...";
  return out;
}

bool same(const Record &a, const Record &b) {
  return a.pass == b.pass && a.kind == b.kind && a.address == b.address && a.has_data == b.has_data &&
         a.length == b.length && a.bytes == b.bytes;
}

void run_side(Entry entry, const uint8_t *data, std::size_t size, Trace &trace) {
  internals::optolink_host_clock() = internals::HostClock{true, kClockStart};
  g_trace = &trace;
  entry(data, size);
  g_trace = nullptr;
}

// The first difference, or an empty string.
std::string compare(const Trace &ref, const Trace &live) {
  const std::size_t n = ref.records.size() < live.records.size() ? ref.records.size() : live.records.size();
  std::size_t passes = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (!same(ref.records[i], live.records[i])) {
      return "record " + std::to_string(i) + " (after " + std::to_string(passes) +
             " pass(es)): reference: " + describe(ref.records[i]) + "; live: " + describe(live.records[i]);
    }
    passes += ref.records[i].pass ? 1 : 0;
  }
  if (ref.records.size() != live.records.size()) {
    const Trace &longer = ref.records.size() > live.records.size() ? ref : live;
    return std::string(&longer == &ref ? "the reference" : "the live side") + " goes on after record " +
           std::to_string(n) + ": " + describe(longer.records[n]);
  }
  return {};
}

// Runs one input on both sides; the first difference or failed oracle, or an
// empty string.
std::string check(const uint8_t *data, std::size_t size) {
  if (size < 1)
    return {};
  const Harness &h = kHarnesses[(data[0] & 0x7F) % 4];
  diff_step_ms = (data[0] & 0x80) ? 1 : 10;
  Trace ref;
  Trace live;
  run_side(h.ref, data + 1, size - 1, ref);
  run_side(h.live, data + 1, size - 1, live);
  if (!ref.oracles.empty())
    return std::string(h.name) + ": reference oracle: " + ref.oracles.front();
  if (!live.oracles.empty())
    return std::string(h.name) + ": live oracle: " + live.oracles.front();
  const std::string diff = compare(ref, live);
  return diff.empty() ? diff : std::string(h.name) + ": " + diff;
}

}  // namespace

uint32_t diff_step_ms = 10;

void diff_note_pass(const uint8_t *written, std::size_t total) {
  Trace &t = *g_trace;
  if (total < t.written)
    t.written = 0;  // a new port within one run
  Record r{true, DiffEvent::RESPONSE, 0, true, 0, std::vector<uint8_t>(written + t.written, written + total)};
  t.written = total;
  t.records.push_back(std::move(r));
}

void diff_note_event(DiffEvent kind, uint16_t address, const uint8_t *data, std::size_t length) {
  Record r{false, kind, address, data != nullptr, length, {}};
  if (data != nullptr)
    r.bytes.assign(data, data + length);
  g_trace->records.push_back(std::move(r));
}

void diff_note_oracle(const char *what) { g_trace->oracles.emplace_back(what); }

#ifdef VITOHOME_FUZZ_LIBFUZZER

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, std::size_t size) {
  const std::string failure = check(data, size);
  if (!failure.empty()) {
    std::fprintf(stderr, "DIFFERENTIAL: %s\n", failure.c_str());
    std::abort();
  }
  return 0;
}

#else

namespace {

int g_fail = 0;
std::size_t g_seeds = 0;

// One input as given.
void replay_one(const char *name, const uint8_t *bytes, std::size_t len) {
  ++g_seeds;
  const std::string failure = check(bytes, len);
  if (!failure.empty()) {
    ++g_fail;
    std::printf("  %-40s FAIL\n    %s\n", name, failure.c_str());
  } else {
    std::printf("  %-40s ok\n", name);
  }
}

// One seed, in 10 ms and in 1 ms steps.
void replay(const char *name, uint8_t tag, const uint8_t *bytes, std::size_t len) {
  std::vector<uint8_t> input;
  input.reserve(len + 1);
  input.push_back(tag);
  input.insert(input.end(), bytes, bytes + len);
  ++g_seeds;
  for (const uint8_t fine : {uint8_t{0}, uint8_t{0x80}}) {
    input[0] = static_cast<uint8_t>((tag & 0x7F) | fine);
    const std::string failure = check(input.data(), input.size());
    if (!failure.empty()) {
      ++g_fail;
      std::printf("  %-40s FAIL (%s steps)\n    %s\n", name, fine ? "1 ms" : "10 ms", failure.c_str());
      return;
    }
  }
  std::printf("  %-40s ok\n", name);
}

}  // namespace

int main(int argc, char **argv) {
  std::printf("== differential: live engines against the reference copy ==\n");
  for (std::size_t i = 0; i < kKwSessionSeedCount; ++i)
    replay(kKwSessionSeeds[i].name, 0, kKwSessionSeeds[i].bytes, kKwSessionSeeds[i].len);
  for (std::size_t i = 0; i < kGwgSessionSeedCount; ++i)
    replay(kGwgSessionSeeds[i].name, 1, kGwgSessionSeeds[i].bytes, kGwgSessionSeeds[i].len);
  for (std::size_t i = 0; i < kRawEngineSeedCount; ++i)
    replay(kRawEngineSeeds[i].name, 2, kRawEngineSeeds[i].bytes, kRawEngineSeeds[i].len);
  for (std::size_t i = 0; i < kSimSessionSeedCount; ++i) {
    std::vector<uint8_t> input(1, 3);
    input.insert(input.end(), kSimSessionSeeds[i].bytes, kSimSessionSeeds[i].bytes + kSimSessionSeeds[i].len);
    replay_one(kSimSessionSeeds[i].name, input.data(), input.size());
  }
  for (std::size_t i = 0; i < kDiffSeedCount; ++i)
    replay_one(kDiffSeeds[i].name, kDiffSeeds[i].bytes, kDiffSeeds[i].len);
  // Further inputs, each already led by its harness byte.
  for (int a = 1; a < argc; ++a) {
    std::FILE *f = std::fopen(argv[a], "rb");
    if (f == nullptr)
      continue;
    static uint8_t buf[64 * 1024];
    const std::size_t len = std::fread(buf, 1, sizeof(buf), f);
    std::fclose(f);
    replay_one(argv[a], buf, len);
  }
  std::printf("differential: %zu seed(s), %d failure(s)\n", g_seeds, g_fail);
  return g_fail == 0 ? 0 : 1;
}

#endif  // VITOHOME_FUZZ_LIBFUZZER
