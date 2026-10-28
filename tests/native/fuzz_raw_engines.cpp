// Fuzz target + deterministic replay runner for the engines' generic
// telegrams: VS2Engine::request() and kwTelegram() (the KW detour),
// VS1Engine::telegram(), GWGEngine::telegram() -- the paths queue_raw_op()
// opens to any function code / type byte. The parsers have their own target
// (fuzz_parser_vs2.cpp); this one drives whole transactions: one operation
// against a scripted controller whose bytes and timing come from the input.
//
// INPUT LAYOUT
//
//   [0] engine    % 4: 0 P300 request, 1 P300 KW detour, 2 KW, 3 GWG
//   [1] code      the function code / type byte
//   [2..3] address
//   [4] length
//   [5] flags     bit 0: a payload rides (its `length` bytes follow)
//   then the controller's script, a sequence of steps:
//     b & 0x80    let (b & 0x7F) * 10 ms pass
//     else        feed the next (b + 1) bytes
//   the engine is pumped after every step.
//
// ORACLES, besides the sanitizers (the response buffer is touched byte by
// byte, so a delivered length past the data is an ASan report):
//   (1) an accepted operation completes exactly once -- one response or one
//       error -- once enough time has passed, and a refused one never;
//   (2) a delivered answer has the shape the operation promises: KW and
//       GWG (outside the EEPROM pairs) <length> bytes, or the one
//       acknowledgement byte after a payload; the detour the same; a P300
//       answer at most the frame's 250 payload bytes;
//   (3) afterwards the engine is idle and takes the next request.
//
// Same two-build arrangement as fuzz_parser_vs2.cpp; seeds in
// fuzz_corpus_raw_engines.h.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "fake_optolink.h"
#include "fuzz_corpus_raw_engines.h"
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

int g_completions = 0;
bool g_shape_ok = true;
uint8_t g_expect_length = 0;  // 0: any length up to g_max_length
uint8_t g_max_length = 0;
volatile uint32_t g_sink = 0;

void on_response(const uint8_t *data, uint8_t length, uint16_t) {
  ++g_completions;
  if (g_expect_length != 0 && length != g_expect_length)
    g_shape_ok = false;
  // A P300 write acknowledgement carries no data, and its length is the
  // frame's DataLength, nothing a consumer reads.
  if (data != nullptr && length > g_max_length)
    g_shape_ok = false;
  if (data != nullptr) {
    for (uint8_t i = 0; i < length; ++i)
      g_sink = g_sink + data[i];
  }
}

void on_error(optolink::OptolinkResult, uint16_t) { ++g_completions; }

template<class E> void pump(E &e, int n = 8) {
  for (int i = 0; i < n; ++i)
    e.loop();
}

template<class E> void run_script(E &e, FakeOptolink &io, const uint8_t *s, std::size_t n) {
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
  e.onResponse(on_response);
  e.onError(on_error);
  e.begin();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, std::size_t size) {
  if (size < 6)
    return 0;
  optolink::optolink_test_clock_freeze();
  const uint8_t engine = data[0] % 4;
  const uint8_t code = data[1];
  const uint16_t address = static_cast<uint16_t>(data[2] << 8 | data[3]);
  const uint8_t length = data[4];
  const bool has_payload = (data[5] & 0x01) != 0;
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
  g_max_length = 250;
  bool accepted = false;
  bool idle_after = false;
  bool next_taken = false;

  FakeOptolink io;
  switch (engine) {
    case 0: {
      auto *e = new optolink::VS2Engine(&io);
      wire(*e);
      accepted = e->request(code, address, length, payload);
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
      accepted = e->kwTelegram(code, address, length, payload);
      run_script(*e, io, script, script_len);
      settle(*e);
      idle_after = !e->isBusy();
      next_taken = e->read(0x00F8, 1);
      delete e;
      break;
    }
    case 2: {
      auto *e = new optolink::VS1Engine(&io);
      wire(*e);
      g_expect_length = has_payload ? 1 : length;
      g_max_length = 255;
      accepted = e->telegram(code, address, length, payload);
      run_script(*e, io, script, script_len);
      settle(*e);
      idle_after = !e->isBusy();
      next_taken = e->read(0x00F8, 1);
      delete e;
      break;
    }
    default: {
      auto *e = new optolink::GWGEngine(&io);
      wire(*e);
      // An EEPROM read (0xAE) delivers its decoded values, `length` of them
      // like every other read; a write mode's ack is one byte.
      g_expect_length = has_payload ? 1 : length;
      g_max_length = 255;
      accepted = e->telegram(code, address, length, payload);
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
  oracle(g_shape_ok, "a delivered answer of the wrong length");
  oracle(idle_after, "the engine is still busy after every timeout");
  oracle(next_taken, "the engine refuses the next request");
  return 0;
}

#ifndef VITOHOME_FUZZ_LIBFUZZER

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

#endif  // VITOHOME_FUZZ_LIBFUZZER
