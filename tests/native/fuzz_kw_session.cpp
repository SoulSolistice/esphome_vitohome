// Fuzz target + deterministic replay runner for KW sessions: up to six
// operations sent back to back on a KW link (VS1Engine) or on a P300 link that
// detours to KW for KW telegrams and GFA reads (VS2Engine), against a scripted
// controller whose bytes and timing come from the input. fuzz_raw_engines.cpp
// drives one telegram; this drives the chain logic between them: the chain
// window, the GFA pause, the P300 -> KW -> P300 switches and the ack checks.
//
// INPUT LAYOUT
//
//   [0] engine     bit 0: 0 KW (VS1Engine), 1 P300 (VS2Engine)
//   [1] ops        b % 6 + 1 operations follow, 5 bytes each:
//         [0] kind     bits 0-2: 0 read, 1 write, 2 GFA read,
//                      3 KW: telegram() without payload / P300: rpc(),
//                      4 KW: telegram() with payload / P300: kwTelegram()
//                        without payload,
//                      5 P300: kwTelegram() with payload / KW: read,
//                      6-7 read
//         [1..2]       address
//         [3] length   bit 7 set: b & 0x7F (past the limits), else b % 8 + 1
//         [4] value    the telegram type, and the first payload byte (the
//                      rest count up)
//   then the controller's script, as in fuzz_raw_engines.cpp:
//     b & 0x80    let (b & 0x7F) * 10 ms pass
//     else        feed the next (b + 1) bytes
//
// ORACLES, besides the sanitizers:
//   (1) every accepted operation completes exactly once, and nothing completes
//       while no operation is in flight;
//   (2) a KW answer carries the request's address and `length` bytes; a KW
//       telegram with payload is answered only by the acknowledgement 00; a
//       P300 answer with data fits MAX_PAYLOAD (a write's acknowledgement
//       echoes the frame's DataLength and carries none);
//   (3) a GFA frame (type 0x6B) sent with no byte from the controller since
//       the last completion is chained onto an answer -- a KW answer on
//       P300 -- and goes out no earlier than GFA_GAP_MS after it;
//   (4) refusals follow the documented limits: a length above MAX_PAYLOAD, a
//       KW detour of 0 bytes or past KW_MAX_PAYLOAD / KW_MAX_LENGTH;
//   (5) afterwards the engine is idle and takes the next request.
//
// Same two-build arrangement as fuzz_parser_vs2.cpp; seeds in
// fuzz_corpus_kw_session.h.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

#include "fuzz_corpus_kw_session.h"
#include "optolink/optolink.h"

namespace optolink = esphome::vitohome::optolink;

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

constexpr int kMaxOps = 6;

enum class Kind : uint8_t { READ, WRITE, GFA, TELEGRAM, TELEGRAM_PAYLOAD, KW, KW_PAYLOAD, RPC };

struct Op {
  Kind kind;
  uint16_t address;
  uint8_t length;
  uint8_t type;
  uint8_t payload[128];
  bool accepted;
  int completions;
};

bool kw_shaped(Kind k, bool p300) {
  return p300 ? (k == Kind::GFA || k == Kind::KW || k == Kind::KW_PAYLOAD) : k != Kind::RPC;
}
bool has_payload(Kind k) {
  return k == Kind::WRITE || k == Kind::TELEGRAM_PAYLOAD || k == Kind::KW_PAYLOAD || k == Kind::RPC;
}
bool sends_gfa_frame(const Op &op) {
  switch (op.kind) {
    case Kind::GFA:
      return true;
    case Kind::TELEGRAM:
    case Kind::TELEGRAM_PAYLOAD:
    case Kind::KW:
    case Kind::KW_PAYLOAD:
      return op.type == optolink::PacketVS1Type.GFA_READ;
    default:
      return false;
  }
}

struct Session {
  bool p300 = false;
  Op ops[kMaxOps];
  int count = 0;
  int next = 0;
  int pending = -1;
  // Oracle (3): the last completion, and whether a byte was fed or read since.
  bool answered = false;
  bool answered_kw = false;
  uint32_t answer_ms = 0;
  bool fed_since_answer = true;
};
Session g_s;
volatile uint32_t g_sink = 0;

// FakeOptolink plus the GFA pause check on every write.
class Line {
 public:
  bool begin() { return true; }
  void end() {}
  std::size_t write(const uint8_t *d, uint8_t n) {
    // A frame sent on a sync starts with ENQ_ACK, after the sync was read.
    if (n > 0 && d[0] == optolink::PacketVS1Type.GFA_READ && g_s.pending >= 0 &&
        sends_gfa_frame(g_s.ops[g_s.pending]) && !g_s.fed_since_answer) {
      oracle(g_s.p300 ? g_s.answered_kw : g_s.answered, "a GFA read chained onto no answer");
      oracle(optolink_millis() - g_s.answer_ms >= optolink::VS1Engine::GFA_GAP_MS,
             "a chained GFA read went out inside GFA_GAP_MS");
    }
    for (uint8_t i = 0; i < n; ++i)
      g_sink = g_sink + d[i];
    return n;
  }
  std::size_t available() const { return in_.size(); }
  uint8_t read() {
    if (in_.empty())
      return 0;
    const uint8_t b = in_.front();
    in_.pop_front();
    g_s.fed_since_answer = true;  // a byte read after the answer is a sync
    return b;
  }
  void feed(const uint8_t *d, std::size_t n) {
    in_.insert(in_.end(), d, d + n);
    if (n > 0)
      g_s.fed_since_answer = true;
  }

 private:
  std::deque<uint8_t> in_;
};

void finish(bool answered) {
  ++g_s.ops[g_s.pending].completions;
  g_s.answered = answered;
  g_s.answered_kw = answered && kw_shaped(g_s.ops[g_s.pending].kind, g_s.p300);
  g_s.answer_ms = optolink_millis();
  g_s.fed_since_answer = false;
  g_s.pending = -1;
}

void on_response(const uint8_t *data, uint8_t length, uint16_t address) {
  oracle(g_s.pending >= 0, "a response with no operation in flight");
  if (g_s.pending < 0)
    return;
  const Op &op = g_s.ops[g_s.pending];
  if (data != nullptr) {
    for (uint8_t i = 0; i < length; ++i)
      g_sink = g_sink + data[i];
  }
  if (kw_shaped(op.kind, g_s.p300)) {
    oracle(address == op.address, "a KW answer for another address");
    if (has_payload(op.kind)) {
      oracle(data != nullptr && length == 1 && data[0] == 0x00, "a KW write answered by something other than 00");
    } else {
      oracle(data != nullptr && length == op.length, "a KW answer of the wrong length");
    }
  } else {
    oracle(data == nullptr || length <= optolink::MAX_PAYLOAD, "a P300 answer longer than MAX_PAYLOAD");
  }
  finish(true);
}

void on_error(optolink::OptolinkResult, uint16_t address) {
  oracle(g_s.pending >= 0, "an error with no operation in flight");
  if (g_s.pending < 0)
    return;
  oracle(address == g_s.ops[g_s.pending].address, "an error for another address");
  finish(false);
}

bool submit_one(optolink::VS1Engine &e, const Op &op) {
  switch (op.kind) {
    case Kind::WRITE:
      return e.write(op.address, op.payload, op.length);
    case Kind::GFA:
      return e.gfaRead(op.address, op.length);
    case Kind::TELEGRAM:
      return e.telegram(op.type, op.address, op.length);
    case Kind::TELEGRAM_PAYLOAD:
      return e.telegram(op.type, op.address, op.length, op.payload);
    default:
      return e.read(op.address, op.length);
  }
}

bool submit_one(optolink::VS2Engine &e, const Op &op) {
  switch (op.kind) {
    case Kind::WRITE:
      return e.write(op.address, op.payload, op.length);
    case Kind::GFA:
      return e.gfaRead(op.address, op.length);
    case Kind::RPC:
      return e.rpc(op.address, op.payload, op.length);
    case Kind::KW:
      return e.kwTelegram(op.type, op.address, op.length);
    case Kind::KW_PAYLOAD:
      return e.kwTelegram(op.type, op.address, op.length, op.payload);
    default:
      return e.read(op.address, op.length);
  }
}

template<class E> void submit(E &e) {
  while (g_s.pending < 0 && g_s.next < g_s.count && !e.isBusy()) {
    Op &op = g_s.ops[g_s.next];
    const int index = g_s.next++;
    op.accepted = submit_one(e, op);
    if (!op.accepted)
      continue;
    oracle(op.length <= optolink::MAX_PAYLOAD, "an operation longer than MAX_PAYLOAD accepted");
    if (g_s.p300 && (op.kind == Kind::GFA || op.kind == Kind::KW || op.kind == Kind::KW_PAYLOAD)) {
      const uint8_t limit =
          op.kind == Kind::KW_PAYLOAD ? optolink::VS2Engine::KW_MAX_PAYLOAD : optolink::VS2Engine::KW_MAX_LENGTH;
      oracle(op.length != 0 && op.length <= limit, "a KW detour outside its length limits accepted");
    }
    g_s.pending = index;
  }
}

template<class E> void pump(E &e, int n) {
  for (int i = 0; i < n; ++i) {
    e.loop();
    submit(e);
  }
}

template<class E> void run(E &e, Line &line, const uint8_t *s, std::size_t n) {
  e.onResponse(on_response);
  e.onError(on_error);
  e.begin();
  submit(e);
  std::size_t i = 0;
  while (i < n) {
    const uint8_t b = s[i++];
    if (b & 0x80) {
      optolink::optolink_test_clock_advance(static_cast<uint32_t>(b & 0x7F) * 10);
    } else {
      const std::size_t take = (static_cast<std::size_t>(b) + 1 < n - i) ? static_cast<std::size_t>(b) + 1 : n - i;
      line.feed(s + i, take);
      i += take;
    }
    pump(e, 4);
  }
  for (int k = 0; k < 600 && (g_s.pending >= 0 || g_s.next < g_s.count || e.isBusy()); ++k) {
    optolink::optolink_test_clock_advance(100);
    pump(e, 2);
  }
  for (int k = 0; k < g_s.count; ++k) {
    const Op &op = g_s.ops[k];
    oracle(op.completions == (op.accepted ? 1 : 0), "an operation did not complete exactly once");
  }
  oracle(g_s.next == g_s.count, "operations left unsubmitted");
  oracle(!e.isBusy(), "the engine is still busy after every timeout");
  oracle(e.read(0x00F8, 1), "the engine refuses the next request");
}

void parse_op(const uint8_t *b, bool p300, Op &op) {
  static constexpr Kind kKw[8] = {Kind::READ, Kind::WRITE, Kind::GFA, Kind::TELEGRAM, Kind::TELEGRAM_PAYLOAD,
                                  Kind::READ, Kind::READ,  Kind::READ};
  static constexpr Kind kP300[8] = {Kind::READ, Kind::WRITE,      Kind::GFA,  Kind::RPC,
                                    Kind::KW,   Kind::KW_PAYLOAD, Kind::READ, Kind::READ};
  op.kind = (p300 ? kP300 : kKw)[b[0] & 0x07];
  op.address = static_cast<uint16_t>(b[1] << 8 | b[2]);
  op.length = (b[3] & 0x80) ? static_cast<uint8_t>(b[3] & 0x7F) : static_cast<uint8_t>(b[3] % 8 + 1);
  op.type = b[4];
  for (uint8_t i = 0; i < op.length; ++i)
    op.payload[i] = static_cast<uint8_t>(b[4] + i);
  op.accepted = false;
  op.completions = 0;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, std::size_t size) {
  if (size < 2)
    return 0;
  const bool p300 = (data[0] & 0x01) != 0;
  const int count = data[1] % kMaxOps + 1;
  std::size_t pos = 2;
  if (size - pos < static_cast<std::size_t>(count) * 5)
    return 0;

  optolink::optolink_test_clock_freeze();
  g_s = Session{};
  g_s.p300 = p300;
  g_s.count = count;
  for (int i = 0; i < count; ++i, pos += 5)
    parse_op(data + pos, p300, g_s.ops[i]);

  Line line;
  if (p300) {
    optolink::VS2Engine e(&line);
    run(e, line, data + pos, size - pos);
  } else {
    optolink::VS1Engine e(&line);
    run(e, line, data + pos, size - pos);
  }
  return 0;
}

#ifndef VITOHOME_FUZZ_LIBFUZZER

int main(int argc, char **argv) {
  std::printf("== KW session fuzz corpus (deterministic replay) ==\n");
  for (std::size_t i = 0; i < kKwSessionSeedCount; ++i) {
    const KwSessionSeed &s = kKwSessionSeeds[i];
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
  std::printf("KW session fuzz corpus: %zu seed(s), %d failure(s)\n", kKwSessionSeedCount, g_fail);
  return g_fail == 0 ? 0 : 1;
}

#endif  // VITOHOME_FUZZ_LIBFUZZER
