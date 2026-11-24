// Fuzz target + deterministic replay runner for KW sessions: up to six
// operations sent back to back on a KW link (the KW exchange) or on a P300
// link that detours to KW for KW telegrams and GFA reads (the P300 exchange), against
// a scripted controller whose bytes and timing come from the input.
// fuzz_raw_engines.cpp drives one telegram; this drives the chain logic
// between them: the chain window, the GFA pause, the P300 -> KW -> P300
// switches and the ack checks.
//
// INPUT LAYOUT
//
//   [0] engine     bit 0: 0 KW (the KW exchange), 1 P300 (the P300 exchange)
//   [1] ops        b % 6 + 1 operations follow, 5 bytes each:
//         [0] kind     bits 0-2: 0 read, 1 write, 2 GFA read,
//                      3 KW: telegram() without payload / P300: rpc(),
//                      4 KW: telegram() with payload / P300: kwTelegram()
//                        without payload,
//                      5 P300: kwTelegram() with payload / KW: read,
//                      6-7 read;
//                      (b & 0xC7) == 0x46: an open frame (frame()) of
//                      [1] bytes (bit 7 set: b & 0x7F, else b % 8 + 1),
//                      counting up from [4], at address 0; bit 3 set: it
//                      collects (answer length 0), else [3] is its answer
//                      length
//         [1..2]       address
//         [3] length   bit 7 set: b & 0x7F (past the limits), else b % 8 + 1
//         [4] value    the telegram type, and the first payload byte (the
//                      rest count up)
//   then the controller's script, as in fuzz_raw_engines.cpp:
//     b & 0x80    let (b & 0x7F) * 10 ms pass (the differential target
//                 also replays it in 1 ms steps)
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
//   (5) afterwards the engine is idle and takes the next request;
//   (6) an open frame is answered at address 0 with its answer length, or
//       collecting with 1..its cap bytes; it is accepted only within its
//       limits; on KW neither the frame nor the request after it is chained
//       (a byte from the controller comes first); on P300 the next byte sent
//       after it is the EOT that re-establishes the session.
//
// Same two-build arrangement as fuzz_parser_vs2.cpp; seeds in
// fuzz_corpus_kw_session.h.
//
// Built with -DVITOHOME_DIFF_SIDE it is one side of fuzz_diff_engines.cpp
// (diff_hooks.h).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

#include "diff_hooks.h"
#include "fake_optolink.h"
#include "fuzz_corpus_kw_session.h"
#ifdef VITOHOME_DIFF_REF
#include "reference/optolink/optolink.h"
namespace optolink = esphome::vitohome::optolink_ref;
#else
#include "optolink_types.h"
namespace optolink = esphome::vitohome::optolink;
#endif
#include "kw_calls.h"

// KW: the KW stack on the live side, VS1Engine on the reference side; in
// the differential target the live side keeps VS1Engine's timing
// (kw_calls::kVs1Timing).
#ifdef VITOHOME_DIFF_REF
template<class Port> using KwEngine = optolink::VS1Engine;
constexpr uint32_t kGfaGapMs = optolink::VS1Engine::GFA_GAP_MS;
#elif defined(VITOHOME_DIFF_SIDE)
template<class Port> using KwEngine = optolink::KwExchange<Port, kw_calls::kVs1Timing>;
constexpr uint32_t kGfaGapMs = optolink::KW_GFA_GAP_MS;
#else
template<class Port> using KwEngine = optolink::KwExchange<Port>;
constexpr uint32_t kGfaGapMs = optolink::KW_GFA_GAP_MS;
#endif

// P300: the P300 stack on the live side, VS2Engine on the reference side;
// as a side of the differential target without the KW silence retry, which
// VS2Engine lacks.
#ifdef VITOHOME_DIFF_REF
template<class Port> using P300Engine = optolink::VS2Engine;
constexpr uint8_t kP300KwMaxPayload = optolink::VS2Engine::KW_MAX_PAYLOAD;
constexpr uint8_t kP300KwMaxLength = optolink::VS2Engine::KW_MAX_LENGTH;
constexpr std::size_t kP300FrameMax = optolink::VS2Engine::FRAME_MAX;
#else
#ifdef VITOHOME_DIFF_SIDE
template<class Port> using P300Engine = optolink::P300Exchange<Port, 0>;
#else
template<class Port> using P300Engine = optolink::P300Exchange<Port>;
#endif
constexpr uint8_t kP300KwMaxPayload = optolink::P300_KW_MAX_PAYLOAD;
constexpr uint8_t kP300KwMaxLength = optolink::P300_KW_MAX_LENGTH;
constexpr std::size_t kP300FrameMax = optolink::P300_OPEN_FRAME_MAX;
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

// The KW frame cap, built from MAX_PAYLOAD so it holds for the live engines
// (KW_FRAME_MAX) and the reference copy alike.
constexpr std::size_t kKwFrameMax = 4 + optolink::MAX_PAYLOAD;

enum class Kind : uint8_t { READ, WRITE, GFA, TELEGRAM, TELEGRAM_PAYLOAD, KW, KW_PAYLOAD, RPC, FRAME };

struct Op {
  Kind kind;
  uint16_t address;
  uint8_t length;
  uint8_t type;
  uint8_t payload[128];
  // FRAME: the frame's byte count (the bytes are `payload`); `length` is the
  // answer length, 0 to collect.
  uint8_t frame_len;
  bool accepted;
  int completions;
};

bool kw_shaped(Kind k, bool p300) {
  return p300 ? (k == Kind::GFA || k == Kind::KW || k == Kind::KW_PAYLOAD) : k != Kind::RPC && k != Kind::FRAME;
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
  // Oracle (6): the last completion was an open frame's.
  bool after_frame = false;
  bool eot_due = false;
};
Session g_s;
volatile uint32_t g_sink = 0;

// FakeOptolink plus the GFA pause check on every write.
class Line {
 public:
  bool begin() { return true; }
  void end() {}
  std::size_t write(const uint8_t *d, uint8_t n) {
    if (n > 0 && g_s.p300 && g_s.eot_due) {
      oracle(d[0] == 0x04, "a P300 frame not followed by the EOT that resets the session");
      g_s.eot_due = false;
    }
    // KW: a frame, and the request after one, wait for the controller's ENQ.
    // The EOT the engine sends while it waits is no request.
    const bool frame_pending = g_s.pending >= 0 && g_s.ops[g_s.pending].kind == Kind::FRAME;
    if (n > 0 && !g_s.p300 && (frame_pending || g_s.after_frame) && !(n == 1 && d[0] == 0x04)) {
      oracle(g_s.fed_since_answer, "an open frame, or the request after it, chained onto an answer");
    }
    // A frame sent on a sync starts with ENQ_ACK, after the sync was read.
    if (n > 0 && d[0] == optolink::PacketVS1Type.GFA_READ && g_s.pending >= 0 &&
        sends_gfa_frame(g_s.ops[g_s.pending]) && !g_s.fed_since_answer) {
      oracle(g_s.p300 ? g_s.answered_kw : g_s.answered, "a GFA read chained onto no answer");
      oracle(optolink_millis() - g_s.answer_ms >= kGfaGapMs, "a chained GFA read went out inside GFA_GAP_MS");
    }
    for (uint8_t i = 0; i < n; ++i)
      g_sink = g_sink + d[i];
    written_.insert(written_.end(), d, d + n);
    return n;
  }
  const std::vector<uint8_t> &written() const { return written_; }
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
  std::vector<uint8_t> written_;
};
Line *g_line = nullptr;

void finish(bool answered) {
  ++g_s.ops[g_s.pending].completions;
  g_s.after_frame = g_s.ops[g_s.pending].kind == Kind::FRAME;
  g_s.eot_due = g_s.after_frame && g_s.p300;
  g_s.answered = answered;
  g_s.answered_kw = answered && kw_shaped(g_s.ops[g_s.pending].kind, g_s.p300);
  g_s.answer_ms = optolink_millis();
  g_s.fed_since_answer = false;
  g_s.pending = -1;
}

void on_response(const uint8_t *data, uint8_t length, uint16_t address) {
  DIFF_EVENT(DiffEvent::RESPONSE, address, data, length);
  oracle(g_s.pending >= 0, "a response with no operation in flight");
  if (g_s.pending < 0)
    return;
  const Op &op = g_s.ops[g_s.pending];
  if (data != nullptr) {
    for (uint8_t i = 0; i < length; ++i)
      g_sink = g_sink + data[i];
  }
  if (op.kind == Kind::FRAME) {
    oracle(address == 0, "an open frame answered at an address");
    if (op.length != 0) {
      oracle(data != nullptr && length == op.length, "an open frame answered with the wrong length");
    } else {
      oracle(data != nullptr && length >= 1 && length <= optolink::MAX_PAYLOAD,
             "a collecting frame answered with nothing or past its cap");
    }
  } else if (kw_shaped(op.kind, g_s.p300)) {
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

void on_error(optolink::OptolinkResult result, uint16_t address) {
  {
    const uint8_t code = static_cast<uint8_t>(result);
    DIFF_EVENT(DiffEvent::ERROR, address, &code, 1);
  }
  oracle(g_s.pending >= 0, "an error with no operation in flight");
  if (g_s.pending < 0)
    return;
  oracle(address == g_s.ops[g_s.pending].address, "an error for another address");
  finish(false);
}

bool submit_one(KwEngine<Line> &e, const Op &op) {
  switch (op.kind) {
    case Kind::WRITE:
      return kw_calls::write(e, op.address, op.payload, op.length);
    case Kind::GFA:
      return kw_calls::gfa_read(e, op.address, op.length);
    case Kind::TELEGRAM:
      return kw_calls::telegram(e, op.type, op.address, op.length);
    case Kind::TELEGRAM_PAYLOAD:
      return kw_calls::telegram(e, op.type, op.address, op.length, op.payload);
    case Kind::FRAME:
      return kw_calls::frame(e, op.payload, op.frame_len, op.length);
    default:
      return kw_calls::read(e, op.address, op.length);
  }
}

bool submit_one(P300Engine<Line> &e, const Op &op) {
  switch (op.kind) {
    case Kind::WRITE:
      return kw_calls::write(e, op.address, op.payload, op.length);
    case Kind::GFA:
      return kw_calls::gfa_read(e, op.address, op.length);
    case Kind::RPC:
      return kw_calls::rpc(e, op.address, op.payload, op.length);
    case Kind::KW:
      return kw_calls::kw_telegram(e, op.type, op.address, op.length);
    case Kind::KW_PAYLOAD:
      return kw_calls::kw_telegram(e, op.type, op.address, op.length, op.payload);
    case Kind::FRAME:
      return kw_calls::frame(e, op.payload, op.frame_len, op.length);
    default:
      return kw_calls::read(e, op.address, op.length);
  }
}

template<class E> void submit(E &e) {
  while (g_s.pending < 0 && g_s.next < g_s.count && !kw_calls::busy(e)) {
    Op &op = g_s.ops[g_s.next];
    const int index = g_s.next++;
    op.accepted = submit_one(e, op);
    if (!op.accepted)
      continue;
    oracle(op.length <= optolink::MAX_PAYLOAD, "an operation longer than MAX_PAYLOAD accepted");
    if (g_s.p300 && (op.kind == Kind::GFA || op.kind == Kind::KW || op.kind == Kind::KW_PAYLOAD)) {
      const uint8_t limit = op.kind == Kind::KW_PAYLOAD ? kP300KwMaxPayload : kP300KwMaxLength;
      oracle(op.length != 0 && op.length <= limit, "a KW detour outside its length limits accepted");
    }
    if (op.kind == Kind::FRAME) {
      const std::size_t frame_max = g_s.p300 ? kP300FrameMax : kKwFrameMax;
      oracle(op.frame_len != 0 && op.frame_len <= frame_max, "an open frame outside its byte limits accepted");
    }
    g_s.pending = index;
  }
}

template<class E> void pump(E &e, int n) {
  for (int i = 0; i < n; ++i) {
    e.loop();
    DIFF_PASS(g_line->written().data(), g_line->written().size());
    submit(e);
  }
}

template<class E> void run(E &e, Line &line, const uint8_t *s, std::size_t n) {
  g_line = &line;
  kw_calls::wire(e, on_response, on_error);
  e.begin();
  submit(e);
  // P300's RESET drains the line before its EOT.
  pump(e, 4);
  std::size_t i = 0;
  while (i < n) {
    const uint8_t b = s[i++];
    if (b & 0x80) {
      optolink::optolink_test_clock_advance(static_cast<uint32_t>(b & 0x7F) * DIFF_STEP_MS);
    } else {
      const std::size_t take = (static_cast<std::size_t>(b) + 1 < n - i) ? static_cast<std::size_t>(b) + 1 : n - i;
      line.feed(s + i, take);
      i += take;
    }
    pump(e, 4);
  }
  for (int k = 0; k < 600 && (g_s.pending >= 0 || g_s.next < g_s.count || kw_calls::busy(e)); ++k) {
    optolink::optolink_test_clock_advance(100);
    pump(e, 2);
  }
  for (int k = 0; k < g_s.count; ++k) {
    const Op &op = g_s.ops[k];
    oracle(op.completions == (op.accepted ? 1 : 0), "an operation did not complete exactly once");
  }
  oracle(g_s.next == g_s.count, "operations left unsubmitted");
  oracle(!kw_calls::busy(e), "the engine is still busy after every timeout");
  oracle(kw_calls::read(e, 0x00F8, 1), "the engine refuses the next request");
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
  op.frame_len = 0;
  if ((b[0] & 0xC7) == 0x46) {
    op.kind = Kind::FRAME;
    op.address = 0;
    op.frame_len = (b[1] & 0x80) ? static_cast<uint8_t>(b[1] & 0x7F) : static_cast<uint8_t>(b[1] % 8 + 1);
    for (uint8_t i = 0; i < op.frame_len; ++i)
      op.payload[i] = static_cast<uint8_t>(b[4] + i);
    if (b[0] & 0x08)
      op.length = 0;
  }
  op.accepted = false;
  op.completions = 0;
}

}  // namespace

#ifdef VITOHOME_DIFF_SIDE
#define FUZZ_ENTRY VITOHOME_DIFF_SIDE
#else
#define FUZZ_ENTRY LLVMFuzzerTestOneInput
#endif

extern "C" int FUZZ_ENTRY(const uint8_t *data, std::size_t size) {
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
    P300Engine<Line> e(&line);
    run(e, line, data + pos, size - pos);
  } else {
    KwEngine<Line> e(&line);
    run(e, line, data + pos, size - pos);
  }
  return 0;
}

#if !defined(VITOHOME_FUZZ_LIBFUZZER) && !defined(VITOHOME_DIFF_SIDE)

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

#endif  // !VITOHOME_FUZZ_LIBFUZZER && !VITOHOME_DIFF_SIDE
