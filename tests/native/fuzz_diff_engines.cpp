// Differential target: each engine fuzz harness runs its seeds against the
// live engines and must match, pass by pass, the trace recorded for that seed
// in fixtures/diff_engines.txt (golden.h): the bytes written in each pass and
// every callback with its address and data, in order. The recorded traces are
// those of the engines before the redesign (docs/design_notes.md, section 3);
// a deliberate change to the wire is re-recorded with VITOHOME_RECORD=1 and
// reviewed as the file's diff.
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
//   (1) every seed's trace equals the recorded one;
//   (2) no harness oracle fails;
//   (3) the links' trace (trace_sink.h) reported;
//   (4) every recorded trace belongs to a seed.
//
// Each harness is compiled with -DVITOHOME_DIFF_SIDE (diff_hooks.h); see
// build_and_run_protocols.sh. The four corpora are replayed, each scripted
// seed in 10 ms and in 1 ms steps, so a timing difference under one step of
// the seeds still shows. Files named on the command line, each led by its
// harness byte, are run for the harness oracles only.

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
#include "golden.h"
#include "optolink_clock.h"
#include "trace_sink.h"

extern "C" int diff_kw_session(const uint8_t *data, std::size_t size);
extern "C" int diff_gwg_session(const uint8_t *data, std::size_t size);
extern "C" int diff_raw_engines(const uint8_t *data, std::size_t size);
extern "C" int diff_sim_session(const uint8_t *data, std::size_t size);

namespace {

namespace internals = esphome::vitohome::optolink::internals;

using Entry = int (*)(const uint8_t *, std::size_t);
struct Harness {
  const char *name;
  Entry entry;
};
constexpr Harness kHarnesses[4] = {
    {"kw session", diff_kw_session},
    {"gwg session", diff_gwg_session},
    {"raw engines", diff_raw_engines},
    {"sim session", diff_sim_session},
};

// Where the clock starts. Not 0: the engines' timers start at 0, so a start
// near 0 would hide a timer left unset.
constexpr uint32_t kClockStart = 1000000;

// One line per record: "> AA BB" a pass that wrote bytes, ". N" N passes that
// wrote none, "<kind> 0xADDR <length>: AA BB" a callback ("-" no bytes,
// "none" no data).
struct Trace {
  Golden::Lines lines;
  std::size_t written = 0;
  uint32_t quiet = 0;
  std::vector<std::string> oracles;

  void flush_quiet() {
    if (this->quiet != 0)
      this->lines.push_back(". " + std::to_string(this->quiet));
    this->quiet = 0;
  }
};

Trace *g_trace = nullptr;
Golden *g_golden = nullptr;

// Runs one input; the trace, or nothing.
Trace run(const uint8_t *data, std::size_t size, const Harness *&h) {
  Trace trace;
  h = &kHarnesses[(data[0] & 0x7F) % 4];
  diff_step_ms = (data[0] & 0x80) ? 1 : 10;
  internals::optolink_host_clock() = internals::HostClock{true, kClockStart};
  g_trace = &trace;
  h->entry(data + 1, size - 1);
  g_trace = nullptr;
  trace.flush_quiet();
  return trace;
}

// The first difference or failed oracle, or an empty string. `key` empty:
// the harness oracles only.
std::string check(const std::string &key, const uint8_t *data, std::size_t size) {
  if (size < 1)
    return {};
  const Harness *h = nullptr;
  const Trace trace = run(data, size, h);
  if (!trace.oracles.empty())
    return std::string(h->name) + ": oracle: " + trace.oracles.front();
  if (key.empty())
    return {};
  const std::string diff = g_golden->check(key, trace.lines);
  return diff.empty() ? diff : std::string(h->name) + ": " + diff;
}

}  // namespace

uint32_t diff_step_ms = 10;

void diff_note_pass(const uint8_t *written, std::size_t total) {
  Trace &t = *g_trace;
  if (total < t.written)
    t.written = 0;  // a new port within one run
  if (total == t.written) {
    ++t.quiet;
    return;
  }
  t.flush_quiet();
  t.lines.push_back("> " + golden_hex(written + t.written, total - t.written));
  t.written = total;
}

void diff_note_event(DiffEvent kind, uint16_t address, const uint8_t *data, std::size_t length) {
  static const char *const kKinds[] = {"response", "error", "timing", "fallback", "late"};
  Trace &t = *g_trace;
  t.flush_quiet();
  char head[48];
  std::snprintf(head, sizeof(head), "%s 0x%04X %zu: ", kKinds[static_cast<int>(kind)], address, length);
  t.lines.push_back(head + (data != nullptr ? golden_hex(data, length) : std::string("none")));
}

void diff_note_oracle(const char *what) { g_trace->oracles.emplace_back(what); }

namespace {

int g_fail = 0;
std::size_t g_seeds = 0;

void report(const char *name, const std::string &failure, const char *steps = nullptr) {
  if (failure.empty()) {
    std::printf("  %-40s ok\n", name);
    return;
  }
  ++g_fail;
  if (steps != nullptr) {
    std::printf("  %-40s FAIL (%s steps)\n    %s\n", name, steps, failure.c_str());
  } else {
    std::printf("  %-40s FAIL\n    %s\n", name, failure.c_str());
  }
}

// One input as given.
void replay_one(const char *harness, const char *name, const uint8_t *bytes, std::size_t len) {
  ++g_seeds;
  report(name, check(std::string(harness) + ": " + name, bytes, len));
}

// One seed, in 10 ms and in 1 ms steps.
void replay(const char *harness, const char *name, uint8_t tag, const uint8_t *bytes, std::size_t len) {
  std::vector<uint8_t> input;
  input.reserve(len + 1);
  input.push_back(tag);
  input.insert(input.end(), bytes, bytes + len);
  ++g_seeds;
  for (const uint8_t fine : {uint8_t{0}, uint8_t{0x80}}) {
    input[0] = static_cast<uint8_t>((tag & 0x7F) | fine);
    const char *steps = fine ? "1 ms" : "10 ms";
    const std::string failure =
        check(std::string(harness) + ": " + name + " (" + steps + ")", input.data(), input.size());
    if (!failure.empty()) {
      report(name, failure, steps);
      return;
    }
  }
  report(name, {});
}

}  // namespace

int main(int argc, char **argv) {
  std::printf("== differential: live engines against the recorded traces ==\n");
  Golden golden("diff_engines");
  g_golden = &golden;
  for (std::size_t i = 0; i < kKwSessionSeedCount; ++i)
    replay("kw", kKwSessionSeeds[i].name, 0, kKwSessionSeeds[i].bytes, kKwSessionSeeds[i].len);
  for (std::size_t i = 0; i < kGwgSessionSeedCount; ++i)
    replay("gwg", kGwgSessionSeeds[i].name, 1, kGwgSessionSeeds[i].bytes, kGwgSessionSeeds[i].len);
  for (std::size_t i = 0; i < kRawEngineSeedCount; ++i)
    replay("raw", kRawEngineSeeds[i].name, 2, kRawEngineSeeds[i].bytes, kRawEngineSeeds[i].len);
  for (std::size_t i = 0; i < kSimSessionSeedCount; ++i) {
    std::vector<uint8_t> input(1, 3);
    input.insert(input.end(), kSimSessionSeeds[i].bytes, kSimSessionSeeds[i].bytes + kSimSessionSeeds[i].len);
    replay_one("sim", kSimSessionSeeds[i].name, input.data(), input.size());
  }
  for (std::size_t i = 0; i < kDiffSeedCount; ++i)
    replay_one("diff", kDiffSeeds[i].name, kDiffSeeds[i].bytes, kDiffSeeds[i].len);
  for (int a = 1; a < argc; ++a) {
    std::FILE *f = std::fopen(argv[a], "rb");
    if (f == nullptr)
      continue;
    static uint8_t buf[64 * 1024];
    const std::size_t len = std::fread(buf, 1, sizeof(buf), f);
    std::fclose(f);
    ++g_seeds;
    report(argv[a], check({}, buf, len));
  }
  g_fail += golden.finish();
  if (trace_sink::count() == 0) {
    std::printf("  the links reported no trace                           FAIL\n");
    ++g_fail;
  }
  std::printf("differential: %zu seed(s), %zu recorded trace(s), %u trace item(s), %d failure(s)\n", g_seeds,
              golden.cases(), static_cast<unsigned>(trace_sink::count()), g_fail);
  return g_fail == 0 ? 0 : 1;
}
