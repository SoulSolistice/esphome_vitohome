// Fuzz target + deterministic replay runner: up to six operations against a
// device simulator (sim/) instead of a script. The device reacts to what the
// engine sends, so an input reaches the states a protocol conversation leads
// to -- the P300 handshake and detour, chains, bursts, retries -- and its
// quirks come from the input too.
//
// INPUT LAYOUT
//
//   [0] protocol  % 3: 0 KW (the KW exchange, SimVitotronic without P300),
//                 1 P300 (VS2Engine, SimVitotronic), 2 GWG (GWGEngine, SimGwg)
//   [1] quirks    bit 0 refuse_chain, bit 1 swallow_ack, bit 2 one extra
//                 answer byte, bit 3 one answer byte missing, bits 4-5
//                 ignore_one_in 0 / 2 / 5 / 13
//   [2] delay     answer_delay_ms = 4 * b
//   [3] window    enq_window_ms = 2 * b
//   [4] dead      b != 0: dead for 40 * b ms of every 20 s
//   [5] ops       b % 6 + 1 operations follow, 4 bytes each:
//         [0] kind     % 4: 0 read, 1 write, 2 KW: GFA read, P300: procedure
//                      call, GWG: EEPROM read, 3 P300: KW detour read, else
//                      read; bit 7: wait (b & 0x70) * 2 ms first
//         [1..2] address (GWG: [2] only)
//         [3] length   % 8 + 1; a write's bytes count up from it
//
// The engine runs in 1 ms passes until every operation completed, at most a
// minute of device time.
//
// ORACLES, besides the sanitizers:
//   (1) every accepted operation completes exactly once;
//   (2) without quirks ([1] and [4] zero, [2] below the engines' answer
//       deadlines) every operation succeeds, and a read returns what the
//       device's memory holds at that address;
//   (3) afterwards the engine is idle and takes the next request.
//
// Same two-build arrangement as fuzz_parser_vs2.cpp; seeds in
// fuzz_corpus_sim_session.h. Built with -DVITOHOME_DIFF_SIDE it is one side
// of fuzz_diff_engines.cpp (diff_hooks.h): both sides build their device
// from the same input, so they face the same device.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "diff_hooks.h"
#include "fuzz_corpus_sim_session.h"
#include "sim/sim_gwg.h"
#include "sim/sim_vitotronic.h"
#ifdef VITOHOME_DIFF_REF
#include "reference/optolink/optolink.h"
namespace optolink = esphome::vitohome::optolink_ref;
#else
#include "optolink/optolink.h"
namespace optolink = esphome::vitohome::optolink;
#endif
#include "kw_calls.h"

// KW: the KW stack on the live side, VS1Engine on the reference side.
#ifdef VITOHOME_DIFF_REF
template<class Port> using KwEngine = optolink::VS1Engine;
#else
template<class Port> using KwEngine = optolink::KwExchange<Port>;
#endif

namespace {

#if !defined(VITOHOME_FUZZ_LIBFUZZER) && !defined(VITOHOME_DIFF_SIDE)
int g_fail = 0;
#endif
#ifndef VITOHOME_DIFF_SIDE
const char *g_case = "(fuzz)";
#endif

void oracle(bool ok, const char *what) {
  if (ok)
    return;
#if defined(VITOHOME_DIFF_SIDE)
  diff_note_oracle(what);
#else
  std::fprintf(stderr, "ORACLE VIOLATED [%s]: %s\n", g_case, what);
#ifdef VITOHOME_FUZZ_LIBFUZZER
  std::abort();
#else
  ++g_fail;
#endif
#endif
}

constexpr int kMaxOps = 6;
constexpr uint32_t kMaxRunMs = 60000;

enum class Proto : uint8_t { KW, P300, GWG };

struct Op {
  uint8_t kind;
  uint32_t wait_ms;
  uint16_t address;
  uint8_t length;
  uint8_t payload[8];
  bool accepted = false;
  int completions = 0;
  bool ok = false;
  std::vector<uint8_t> data;
};

struct Session {
  Op ops[kMaxOps];
  int count = 0;
  int pending = -1;
};
Session g_s;

void on_response(const uint8_t *data, uint8_t length, uint16_t address) {
  DIFF_EVENT(DiffEvent::RESPONSE, address, data, length);
  oracle(g_s.pending >= 0, "a response with no operation in flight");
  if (g_s.pending < 0)
    return;
  Op &op = g_s.ops[g_s.pending];
  ++op.completions;
  op.ok = true;
  op.data.assign(data, data != nullptr ? data + length : data);
  g_s.pending = -1;
}

void on_error(optolink::OptolinkResult result, uint16_t address) {
  {
    const uint8_t code = static_cast<uint8_t>(result);
    DIFF_EVENT(DiffEvent::ERROR, address, &code, 1);
  }
  oracle(g_s.pending >= 0, "an error with no operation in flight");
  if (g_s.pending < 0)
    return;
  ++g_s.ops[g_s.pending].completions;
  g_s.pending = -1;
}

bool submit_one(KwEngine<sim::SimVitotronic> &e, Op &op) {
  switch (op.kind) {
    case 1:
      return kw_calls::write(e, op.address, op.payload, op.length);
    case 2:
      return kw_calls::gfa_read(e, op.address, op.length);
    default:
      return kw_calls::read(e, op.address, op.length);
  }
}

bool submit_one(optolink::VS2Engine &e, Op &op) {
  switch (op.kind) {
    case 1:
      return e.write(op.address, op.payload, op.length);
    case 2:
      return e.rpc(op.address, op.payload, op.length);
    case 3:
      return e.kwTelegram(optolink::PacketVS1Type.READ, op.address, op.length);
    default:
      return e.read(op.address, op.length);
  }
}

bool submit_one(optolink::GWGEngine &e, Op &op) {
  switch (op.kind) {
    case 1:
      return e.write(op.address, op.payload, op.length);
    case 2:
      return e.read(op.address, op.length, optolink::GWGAccessMode::EEPROM);
    default:
      return e.read(op.address, op.length);
  }
}

// What a read returns from the device's memory when nothing goes wrong.
std::vector<uint8_t> expected(const sim::SimVitotronic &dev, const Op &op, Proto proto) {
  std::vector<uint8_t> want(op.length);
  if (op.kind == 1) {
    return {};  // a write's acknowledgement
  }
  if (proto == Proto::P300 && op.kind == 2)
    return dev.config().rpc_answer;
  const bool gfa = proto == Proto::KW && op.kind == 2;
  (gfa ? dev.gfa : dev.memory).read(op.address, op.length, want.data());
  return want;
}
std::vector<uint8_t> expected(const sim::SimGwg &dev, const Op &op, Proto) {
  if (op.kind == 1)
    return {};
  std::vector<uint8_t> want(op.length);
  const std::size_t mode = op.kind == 2 ? 2 : 0;
  for (uint8_t i = 0; i < op.length; ++i)
    want[i] = dev.memory[mode][static_cast<uint8_t>(op.address + i)];
  return want;
}

template<class E, class Dev> void run(E &e, Dev &dev, Proto proto, bool benign) {
  kw_calls::wire(e, on_response, on_error);
  e.begin();
  int next = 0;
  uint32_t wait_until = optolink_millis();
  const uint32_t start = optolink_millis();
  while (optolink_millis() - start < kMaxRunMs && (next < g_s.count || g_s.pending >= 0 || kw_calls::busy(e))) {
    if (g_s.pending < 0 && next < g_s.count && !kw_calls::busy(e)) {
      Op &op = g_s.ops[next];
      if (wait_until == 0) {
        wait_until = optolink_millis() + op.wait_ms;
      }
      if (static_cast<int32_t>(optolink_millis() - wait_until) >= 0) {
        op.accepted = submit_one(e, op);
        if (op.accepted)
          g_s.pending = next;
        ++next;
        wait_until = 0;
      }
    }
    e.loop();
    DIFF_PASS(dev.written().data(), dev.written().size());
    if (g_s.pending < 0 && next > 0) {
      Op &done = g_s.ops[next - 1];
      if (benign && done.accepted && done.completions == 1) {
        oracle(done.ok, "an operation failed against a device without quirks");
        if (done.ok && done.kind != 1)
          oracle(done.data == expected(dev, done, proto), "a read returned what the device does not hold");
      }
    }
    optolink::optolink_test_clock_advance(1);
  }
  for (int k = 0; k < g_s.count; ++k) {
    const Op &op = g_s.ops[k];
    oracle(op.completions == (op.accepted ? 1 : 0), "an operation did not complete exactly once");
  }
  oracle(!kw_calls::busy(e), "the engine is still busy after a minute");
}

}  // namespace

#ifdef VITOHOME_DIFF_SIDE
#define FUZZ_ENTRY VITOHOME_DIFF_SIDE
#else
#define FUZZ_ENTRY LLVMFuzzerTestOneInput
#endif

extern "C" int FUZZ_ENTRY(const uint8_t *data, std::size_t size) {
  if (size < 6)
    return 0;
  const Proto proto = static_cast<Proto>(data[0] % 3);
  const int count = data[5] % kMaxOps + 1;
  if (size < 6 + static_cast<std::size_t>(count) * 4)
    return 0;

  optolink::optolink_test_clock_freeze();
  g_s = Session{};
  g_s.count = count;
  for (int i = 0; i < count; ++i) {
    const uint8_t *b = data + 6 + i * 4;
    Op &op = g_s.ops[i];
    op.kind = b[0] & 0x03;
    op.wait_ms = (b[0] & 0x80) ? static_cast<uint32_t>(b[0] & 0x70) * 2 : 0;
    op.address = proto == Proto::GWG ? b[2] : static_cast<uint16_t>(b[1] << 8 | b[2]);
    op.length = static_cast<uint8_t>(b[3] % 8 + 1);
    for (uint8_t k = 0; k < op.length; ++k)
      op.payload[k] = static_cast<uint8_t>(b[3] + k);
  }

  sim::Quirks q = proto == Proto::GWG ? sim::GwgConfig::gwg_quirks() : sim::VitotronicConfig::vitotronic_quirks();
  q.refuse_chain = (data[1] & 0x01) != 0;
  q.swallow_ack = (data[1] & 0x02) != 0;
  q.answer_extra = (data[1] & 0x04) ? 1 : 0;
  q.answer_missing = (data[1] & 0x08) ? 1 : 0;
  static constexpr uint32_t kIgnore[4] = {0, 2, 5, 13};
  q.ignore_one_in = kIgnore[(data[1] >> 4) & 0x03];
  if (data[2] != 0)
    q.answer_delay_ms = 4u * data[2];
  if (data[3] != 0)
    q.enq_window_ms = 2u * data[3];
  if (data[4] != 0) {
    q.dead_period_ms = 20000;
    q.dead_ms = 40u * data[4];
  }
  // The shortest answer deadline of the three engines: GWG's.
  const bool benign = (data[1] & 0x3F) == 0 && data[4] == 0 && q.answer_delay_ms < 200;

  if (proto == Proto::GWG) {
    sim::GwgConfig c;
    c.quirks = q;
    sim::SimGwg dev(c);
    optolink::GWGEngine e(&dev);
    run(e, dev, proto, benign);
    oracle(e.read(0x05, 1), "the engine refuses the next request");
  } else {
    sim::VitotronicConfig c;
    c.quirks = q;
    c.p300 = proto == Proto::P300;
    sim::SimVitotronic dev(c);
    if (proto == Proto::P300) {
      optolink::VS2Engine e(&dev);
      run(e, dev, proto, benign);
      oracle(e.read(0x00F8, 1), "the engine refuses the next request");
    } else {
      KwEngine<sim::SimVitotronic> e(&dev);
      run(e, dev, proto, benign);
      oracle(kw_calls::read(e, 0x00F8, 1), "the engine refuses the next request");
    }
  }
  return 0;
}

#if !defined(VITOHOME_FUZZ_LIBFUZZER) && !defined(VITOHOME_DIFF_SIDE)

int main(int argc, char **argv) {
  std::printf("== simulator session fuzz corpus (deterministic replay) ==\n");
  for (std::size_t i = 0; i < kSimSessionSeedCount; ++i) {
    const SimSessionSeed &s = kSimSessionSeeds[i];
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
  std::printf("simulator session fuzz corpus: %zu seed(s), %d failure(s)\n", kSimSessionSeedCount, g_fail);
  return g_fail == 0 ? 0 : 1;
}

#endif  // !VITOHOME_FUZZ_LIBFUZZER && !VITOHOME_DIFF_SIDE
