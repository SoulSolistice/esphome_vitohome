// Fuzz target + deterministic replay runner for whole GWG sessions: up to six
// reads and writes in any access mode, sent back to back the way the hub sends
// them, against a scripted device whose bytes and timing come from the input.
// fuzz_raw_engines.cpp drives one generic telegram; this drives the session
// logic around it: the burst window, the burst retry and fallback, EEPROM pair
// runs, the ENQ-to-send delay, the response deadline and the late-answer
// report.
//
// INPUT LAYOUT
//
//   [0] switches   bit 0: burst-sent requests keep the 0x01 (default: omit)
//                  bit 1: EOT before the write payload
//                  bits 2-4: ENQ-to-send delay, kDelays[]
//                  bits 5-7: response deadline, kDeadlines[]
//   [1] ops        b % 6 + 1 operations follow, 5 bytes each:
//         [0] kind     bits 0-2 access mode, bit 3 write, bit 4 16-bit address
//         [1..2]       address (high byte ignored without bit 4)
//         [3] length   bit 7 set: b & 0x7F (past MAX_PAYLOAD), else b % 8 + 1
//         [4] value    first payload byte of a write, the rest count up
//   then the device's script, as in fuzz_raw_engines.cpp:
//     b & 0x80    let (b & 0x7F) * 10 ms pass
//     else        feed the next (b + 1) bytes
//   The engine is pumped after every step; whenever it is idle the next
//   operation is submitted.
//
// ORACLES, besides the sanitizers:
//   (1) every accepted operation completes exactly once, and nothing completes
//       while no operation is in flight;
//   (2) a completion carries its operation's address; a read delivers
//       `length` bytes (an EEPROM read one value per address), a write one
//       acknowledgement byte;
//   (3) refusals follow the documented rules: a KMBUS write, an EEPROM write
//       of more than one byte, a read longer than MAX_PAYLOAD and an EEPROM
//       run past 0xFF are refused;
//   (4) with a delay set, a request sent on an ENQ went out no earlier than
//       the delay after it; at delay 0 the engine never holds;
//   (5) the timing report fires for answered reads only, with their address;
//       the burst fallback fires at most once;
//   (6) a late-answer report names an address of the session, stores at most
//       8 of the late bytes, states the configured deadline and never covers
//       a telegram that received exactly its answer;
//   (7) an idle engine writes nothing to the line;
//   (8) afterwards the engine is idle and takes the next request.
//
// Same two-build arrangement as fuzz_parser_vs2.cpp; seeds in
// fuzz_corpus_gwg_session.h.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "fake_optolink.h"
#include "fuzz_corpus_gwg_session.h"
#include "optolink/optolink.h"

namespace optolink = esphome::vitohome::optolink;
using optolink::GWGAccessMode;
using optolink::GWGEngine;

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

constexpr uint32_t kDelays[8] = {0, 0, 0, 5, 15, 30, 50, optolink::GWG_ENQ_SEND_DELAY_MAX_MS};
constexpr uint32_t kDeadlines[8] = {GWGEngine::RESPONSE_TIMEOUT_MS,
                                    GWGEngine::RESPONSE_TIMEOUT_MS,
                                    optolink::GWG_RESPONSE_TIMEOUT_MIN_MS,
                                    100,
                                    400,
                                    700,
                                    1000,
                                    optolink::GWG_RESPONSE_TIMEOUT_MAX_MS};
constexpr int kMaxOps = 6;

struct Op {
  GWGAccessMode mode;
  bool write;
  uint16_t address;
  uint8_t length;
  uint8_t payload[128];
  bool accepted;
  int completions;
};

struct Session {
  GWGEngine *engine = nullptr;
  Op ops[kMaxOps];
  int count = 0;
  int next = 0;      // next operation to submit
  int pending = -1;  // operation in flight
  uint32_t delay = 0;
  uint32_t deadline = 0;
  int fallbacks = 0;
};
Session g_s;
volatile uint32_t g_sink = 0;

bool session_address(uint16_t address) {
  for (int i = 0; i < g_s.count; ++i) {
    if (g_s.ops[i].accepted && g_s.ops[i].address == address)
      return true;
  }
  return false;
}

void check_paced_send() {
  const GWGEngine &e = *g_s.engine;
  if (e.lastSendWasEnqPaced())
    oracle(e.lastEnqToSendMs() >= g_s.delay, "a request went out before the ENQ-to-send delay");
}

void on_response(const uint8_t *data, uint8_t length, uint16_t address) {
  oracle(g_s.pending >= 0, "a response with no operation in flight");
  if (g_s.pending < 0)
    return;
  Op &op = g_s.ops[g_s.pending];
  ++op.completions;
  oracle(address == op.address, "a response for another address");
  oracle(data != nullptr, "a response without data");
  oracle(length == (op.write ? 1 : op.length), "a response of the wrong length");
  if (data != nullptr) {
    for (uint8_t i = 0; i < length; ++i)
      g_sink = g_sink + data[i];
  }
  check_paced_send();
  g_s.pending = -1;
}

void on_error(optolink::OptolinkResult, uint16_t address) {
  oracle(g_s.pending >= 0, "an error with no operation in flight");
  if (g_s.pending < 0)
    return;
  Op &op = g_s.ops[g_s.pending];
  ++op.completions;
  oracle(address == op.address, "an error for another address");
  g_s.pending = -1;
}

void on_timing(uint32_t, uint32_t, uint16_t address) {
  oracle(g_s.pending >= 0 && !g_s.ops[g_s.pending].write, "a timing report outside an answered read");
  if (g_s.pending >= 0)
    oracle(address == g_s.ops[g_s.pending].address, "a timing report for another address");
}

void on_fallback(uint8_t) { oracle(++g_s.fallbacks == 1, "the burst fallback fired twice"); }

void on_late(const GWGEngine::LateAnswer &late) {
  oracle(session_address(late.address), "a late-answer report for an address nobody asked");
  oracle(late.late_stored <= late.late.size() && late.late_stored <= late.late_count,
         "a late-answer report stores more than it saw");
  // An answer of exactly `expected` bytes completes; a longer one times out.
  oracle(late.received != late.expected, "a late-answer report for a telegram that got its answer");
  oracle(late.deadline_ms == g_s.deadline, "a late-answer report states another deadline");
  oracle(late.received != 0 || late.late_count != 0, "an empty late-answer report");
}

// Submits operations until one is accepted or none is left.
void submit() {
  GWGEngine &e = *g_s.engine;
  while (g_s.pending < 0 && g_s.next < g_s.count && !e.isBusy()) {
    Op &op = g_s.ops[g_s.next];
    const int index = g_s.next++;
    if (op.write) {
      op.accepted = e.write(op.address, op.payload, op.length, op.mode);
      if (op.accepted) {
        oracle(optolink::gwgModeIsWritable(op.mode), "a write accepted in a mode without a write type");
        oracle(op.mode != GWGAccessMode::EEPROM || op.length == 1, "an EEPROM write of more than one byte");
      }
    } else {
      op.accepted = e.read(op.address, op.length, op.mode);
      if (op.accepted) {
        oracle(op.length <= optolink::MAX_PAYLOAD, "a read longer than MAX_PAYLOAD accepted");
        oracle(op.mode != GWGAccessMode::EEPROM || (op.length != 0 && op.address + op.length - 1 <= 0xFF),
               "an EEPROM run past 0xFF accepted");
      }
    }
    if (op.accepted)
      g_s.pending = index;
  }
}

void pump(FakeOptolink &io, int n) {
  GWGEngine &e = *g_s.engine;
  for (int i = 0; i < n; ++i) {
    const bool idle_before = !e.isBusy();
    const std::size_t written_before = io.written().size();
    e.loop();
    if (idle_before && !e.isBusy())
      oracle(io.written().size() == written_before, "an idle engine wrote to the line");
    if (g_s.delay == 0)
      oracle(!e.isHoldingEnq(), "the engine holds an ENQ at delay 0");
    submit();
  }
}

void parse_op(const uint8_t *b, Op &op) {
  op.mode = static_cast<GWGAccessMode>(b[0] & 0x07);
  op.write = (b[0] & 0x08) != 0;
  op.address = static_cast<uint16_t>(((b[0] & 0x10) ? b[1] << 8 : 0) | b[2]);
  op.length = (b[3] & 0x80) ? static_cast<uint8_t>(b[3] & 0x7F) : static_cast<uint8_t>(b[3] % 8 + 1);
  for (uint8_t i = 0; i < op.length; ++i)
    op.payload[i] = static_cast<uint8_t>(b[4] + i);
  op.accepted = false;
  op.completions = 0;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, std::size_t size) {
  if (size < 2)
    return 0;
  const uint8_t sw = data[0];
  const int count = data[1] % kMaxOps + 1;
  std::size_t pos = 2;
  if (size - pos < static_cast<std::size_t>(count) * 5)
    return 0;

  optolink::optolink_test_clock_freeze();
  g_s = Session{};
  g_s.count = count;
  for (int i = 0; i < count; ++i, pos += 5)
    parse_op(data + pos, g_s.ops[i]);
  g_s.delay = kDelays[(sw >> 2) & 0x07];
  g_s.deadline = kDeadlines[(sw >> 5) & 0x07];

  FakeOptolink io;
  GWGEngine engine(&io);
  g_s.engine = &engine;
  engine.onResponse(on_response);
  engine.onError(on_error);
  engine.onTiming(on_timing);
  engine.onBurstFallback(on_fallback);
  engine.onLateAnswer(on_late);
  engine.setBurstOmitsEnqAck((sw & 0x01) == 0);
  engine.setWriteEotBeforePayload((sw & 0x02) != 0);
  engine.setEnqSendDelayMs(g_s.delay);
  engine.setResponseTimeoutMs(g_s.deadline);
  engine.begin();
  submit();

  const uint8_t *s = data + pos;
  const std::size_t n = size - pos;
  std::size_t i = 0;
  while (i < n) {
    const uint8_t b = s[i++];
    if (b & 0x80) {
      optolink::optolink_test_clock_advance(static_cast<uint32_t>(b & 0x7F) * 10);
    } else {
      const std::size_t take = (static_cast<std::size_t>(b) + 1 < n - i) ? static_cast<std::size_t>(b) + 1 : n - i;
      io.feed(std::vector<uint8_t>(s + i, s + i + take));
      i += take;
    }
    pump(io, 4);
  }

  // A silent line from here: every remaining operation runs into its
  // watchdog, and the late-answer watch closes.
  for (int k = 0; k < 600 && (g_s.pending >= 0 || g_s.next < g_s.count || engine.isBusy()); ++k) {
    optolink::optolink_test_clock_advance(100);
    pump(io, 2);
  }
  for (int k = 0; k < 30; ++k) {
    optolink::optolink_test_clock_advance(100);
    pump(io, 1);
  }

  for (int k = 0; k < count; ++k) {
    const Op &op = g_s.ops[k];
    oracle(op.completions == (op.accepted ? 1 : 0), "an operation did not complete exactly once");
  }
  oracle(g_s.next == count, "operations left unsubmitted");
  oracle(!engine.isBusy(), "the engine is still busy after every timeout");
  oracle(engine.read(0x05, 1), "the engine refuses the next request");
  g_s.engine = nullptr;
  return 0;
}

#ifndef VITOHOME_FUZZ_LIBFUZZER

int main(int argc, char **argv) {
  std::printf("== GWG session fuzz corpus (deterministic replay) ==\n");
  for (std::size_t i = 0; i < kGwgSessionSeedCount; ++i) {
    const GwgSessionSeed &s = kGwgSessionSeeds[i];
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
    const int before = g_fail;
    LLVMFuzzerTestOneInput(buf, len);
    std::printf("  %-40s %4zu bytes  %s\n", argv[a], len, g_fail == before ? "ok" : "FAIL");
  }
  std::printf("GWG session fuzz corpus: %zu seed(s), %d failure(s)\n", kGwgSessionSeedCount, g_fail);
  return g_fail == 0 ? 0 : 1;
}

#endif  // VITOHOME_FUZZ_LIBFUZZER
