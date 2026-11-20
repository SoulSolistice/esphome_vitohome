// Fuzz target + deterministic replay runner for the engines' generic
// telegrams: VS2Engine::request() and kwTelegram() (the KW detour), a KW
// request on the KW exchange, GWGEngine::telegram() -- the paths queue_raw_op()
// opens to any function code / type byte -- and the open frames on every
// engine (queue_raw_frame()). The parsers have their own target
// (fuzz_parser_vs2.cpp); this one drives whole transactions: one operation
// against a scripted controller whose bytes and timing come from the input.
//
// INPUT LAYOUT
//
//   [0] engine    % 4: 0 P300 request, 1 P300 KW detour, 2 KW, 3 GWG
//   [1] code      the function code / type byte
//   [2..3] address
//   [4] length
//   [5] flags     bit 0: a payload rides (its `length` bytes follow);
//                 high nibble 4: an open frame instead, the payload its bytes
//                 and [1] its answer length (0: it collects), on the P300
//                 engine for 0 and 1
//   then the controller's script, a sequence of steps:
//     b & 0x80    let (b & 0x7F) * 10 ms pass (the differential target
//                 also replays it in 1 ms steps)
//     else        feed the next (b + 1) bytes
//   the engine is pumped after every step.
//
// ORACLES, besides the sanitizers (the response buffer is touched byte by
// byte, so a delivered length past the data is an ASan report):
//   (1) an accepted operation completes exactly once -- one response or one
//       error -- once enough time has passed, and a refused one never;
//   (2) a delivered answer has the shape the operation promises: KW and
//       GWG (outside the EEPROM pairs) <length> bytes, or the one
//       acknowledgement byte after a payload; the detour the same; any
//       answer at most MAX_PAYLOAD bytes, and a read asking for more is
//       refused;
//   (3) afterwards the engine is idle and takes the next request;
//   (4) a KW telegram with payload, native or over the detour, delivers only
//       the acknowledgement 00 -- any other byte is an error;
//   (5) an open frame is accepted only with 1..its engine's frame cap bytes
//       and an answer length of at most MAX_PAYLOAD; it delivers that many
//       bytes, or collecting 1..MAX_PAYLOAD.
//
// Same two-build arrangement as fuzz_parser_vs2.cpp; seeds in
// fuzz_corpus_raw_engines.h.
//
// Built with -DVITOHOME_DIFF_SIDE it is one side of fuzz_diff_engines.cpp
// (diff_hooks.h).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "diff_hooks.h"
#include "fake_optolink.h"
#include "fuzz_corpus_raw_engines.h"
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

// The frame caps, built from MAX_PAYLOAD so they hold for the live engines
// (KW_FRAME_MAX, GWG_FRAME_MAX) and the reference copy alike.
constexpr std::size_t kKwFrameMax = 4 + optolink::MAX_PAYLOAD;
constexpr std::size_t kGwgFrameMax = 5 + optolink::MAX_PAYLOAD;

int g_completions = 0;
bool g_shape_ok = true;
uint8_t g_expect_length = 0;  // 0: any length up to g_max_length
uint8_t g_max_length = 0;
// A KW telegram with payload (native or over the detour) is acknowledged with
// 00; anything else is an error, never a delivered answer.
bool g_ack_must_be_zero = false;
// A collecting frame delivers at least one byte.
bool g_collect = false;
volatile uint32_t g_sink = 0;

void on_response(const uint8_t *data, uint8_t length, uint16_t address) {
  DIFF_EVENT(DiffEvent::RESPONSE, address, data, length);
  ++g_completions;
  if (g_expect_length != 0 && length != g_expect_length)
    g_shape_ok = false;
  // A P300 write acknowledgement carries no data, and its length is the
  // frame's DataLength, nothing a consumer reads.
  if (data != nullptr && length > g_max_length)
    g_shape_ok = false;
  if (g_ack_must_be_zero && (data == nullptr || length != 1 || data[0] != 0x00))
    g_shape_ok = false;
  if (g_collect && (data == nullptr || length == 0))
    g_shape_ok = false;
  if (data != nullptr) {
    for (uint8_t i = 0; i < length; ++i)
      g_sink = g_sink + data[i];
  }
}

void on_error(optolink::OptolinkResult result, uint16_t address) {
  {
    const uint8_t code = static_cast<uint8_t>(result);
    DIFF_EVENT(DiffEvent::ERROR, address, &code, 1);
  }
  ++g_completions;
}

FakeOptolink *g_io = nullptr;

template<class E> void pump(E &e, int n = 8) {
  for (int i = 0; i < n; ++i) {
    e.loop();
    DIFF_PASS(g_io->written().data(), g_io->written().size());
  }
}

template<class E> void run_script(E &e, FakeOptolink &io, const uint8_t *s, std::size_t n) {
  std::size_t i = 0;
  while (i < n) {
    const uint8_t b = s[i++];
    if (b & 0x80) {
      optolink::optolink_test_clock_advance(static_cast<uint32_t>(b & 0x7F) * DIFF_STEP_MS);
    } else {
      const std::size_t take = (static_cast<std::size_t>(b) + 1 < n - i) ? static_cast<std::size_t>(b) + 1 : n - i;
      io.feed(std::vector<uint8_t>(s + i, s + i + take));
      i += take;
    }
    pump(e);
  }
}

// Long enough for every engine timeout to fire on a silent line.
template<class E> void settle(E &e) {
  for (int i = 0; i < 200; ++i) {
    optolink::optolink_test_clock_advance(100);
    pump(e, 2);
  }
}

template<class E> void wire(E &e) {
  kw_calls::wire(e, on_response, on_error);
  e.begin();
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
  optolink::optolink_test_clock_freeze();
  const uint8_t engine = data[0] % 4;
  const uint8_t code = data[1];
  const uint16_t address = static_cast<uint16_t>(data[2] << 8 | data[3]);
  const uint8_t length = data[4];
  const bool has_payload = (data[5] & 0x01) != 0;
  const bool frame = (data[5] & 0xF0) == 0x40;
  std::size_t pos = 6;
  const uint8_t *payload = nullptr;
  if (has_payload) {
    if (size - pos < length)
      return 0;
    payload = data + pos;
    pos += length;
  }
  const uint8_t *script = data + pos;
  const std::size_t script_len = size - pos;

  g_completions = 0;
  g_shape_ok = true;
  g_expect_length = 0;
  g_max_length = optolink::MAX_PAYLOAD;
  g_ack_must_be_zero = false;
  g_collect = false;
  bool accepted = false;
  bool idle_after = false;
  bool next_taken = false;

  FakeOptolink io;
  g_io = &io;
  const std::size_t frame_max = engine == 2   ? kKwFrameMax
                                : engine == 3 ? kGwgFrameMax
                                              : std::size_t{optolink::VS2Engine::FRAME_MAX};
  if (frame) {
    // The answer length is the code byte; a refused frame never completes.
    g_expect_length = code;
    g_collect = code == 0;
  }
  switch (frame ? (engine == 1 ? 0 : engine) : engine) {
    case 0: {
      auto *e = new optolink::VS2Engine(&io);
      wire(*e);
      accepted = frame ? e->frame(payload, length, code) : e->request(code, address, length, payload);
      run_script(*e, io, script, script_len);
      settle(*e);
      idle_after = !e->isBusy();
      uint8_t n = 0;
      e->errorPayload(n);
      oracle(n <= optolink::VS2Engine::ERROR_PAYLOAD_MAX, "error payload longer than its buffer");
      next_taken = e->read(0x00F8, 1);
      delete e;
      break;
    }
    case 1: {
      auto *e = new optolink::VS2Engine(&io);
      wire(*e);
      g_expect_length = has_payload ? 1 : length;
      g_max_length = optolink::VS2Engine::KW_MAX_LENGTH;
      g_ack_must_be_zero = has_payload;
      accepted = e->kwTelegram(code, address, length, payload);
      run_script(*e, io, script, script_len);
      settle(*e);
      idle_after = !e->isBusy();
      next_taken = e->read(0x00F8, 1);
      delete e;
      break;
    }
    case 2: {
      auto *e = new KwEngine<FakeOptolink>(&io);
      wire(*e);
      if (!frame) {
        g_expect_length = has_payload ? 1 : length;
        g_ack_must_be_zero = has_payload;
      }
      accepted =
          frame ? kw_calls::frame(*e, payload, length, code) : kw_calls::telegram(*e, code, address, length, payload);
      run_script(*e, io, script, script_len);
      settle(*e);
      idle_after = !kw_calls::busy(*e);
      next_taken = kw_calls::read(*e, 0x00F8, 1);
      delete e;
      break;
    }
    default: {
      auto *e = new optolink::GWGEngine(&io);
      wire(*e);
      // An EEPROM read (0xAE) delivers its decoded values, `length` of them
      // like every other read; a write mode's ack is one byte.
      if (!frame)
        g_expect_length = has_payload ? 1 : length;
      accepted = frame ? e->frame(payload, length, code) : e->telegram(code, address, length, payload);
      run_script(*e, io, script, script_len);
      settle(*e);
      idle_after = !e->isBusy();
      next_taken = e->read(0x05, 1);
      delete e;
      break;
    }
  }

  if (accepted) {
    oracle(g_completions == 1, "an accepted operation did not complete exactly once");
  } else {
    oracle(g_completions == 0, "a refused operation completed");
  }
  if (frame) {
    if (accepted)
      oracle(payload != nullptr && length != 0 && length <= frame_max && code <= optolink::MAX_PAYLOAD,
             "an open frame outside its limits was accepted");
  } else if (payload == nullptr && length > g_max_length) {
    oracle(!accepted, "a read longer than the answer can be was accepted");
  }
  oracle(g_shape_ok, "a delivered answer of the wrong length");
  oracle(idle_after, "the engine is still busy after every timeout");
  oracle(next_taken, "the engine refuses the next request");
  return 0;
}

#if !defined(VITOHOME_FUZZ_LIBFUZZER) && !defined(VITOHOME_DIFF_SIDE)

int main(int argc, char **argv) {
  std::printf("== raw engines fuzz corpus (deterministic replay) ==\n");
  for (std::size_t i = 0; i < kRawEngineSeedCount; ++i) {
    const RawEngineSeed &s = kRawEngineSeeds[i];
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
  std::printf("raw engines fuzz corpus: %zu seed(s), %d failure(s)\n", kRawEngineSeedCount, g_fail);
  return g_fail == 0 ? 0 : 1;
}

#endif  // !VITOHOME_FUZZ_LIBFUZZER && !VITOHOME_DIFF_SIDE
