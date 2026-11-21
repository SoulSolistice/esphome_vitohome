// The KW link's own timing: what VS1Engine did one pass later happens in the
// pass that allows it, and a telegram met by silence goes out once more on
// the device's next ENQ (a VScotHO1_72 left 17 of 6066 telegrams sent on an
// ENQ unanswered, 2026-11-21).
//
// Pinned below, on a frozen clock and counted loop() calls:
//   (1) the 01 and the telegram go out in the loop() that reads the ENQ;
//   (2) a telegram chained onto an answer goes out from submit(), with no
//       loop() between;
//   (3) a chained GFA read still waits its 25 ms, then goes out in the loop()
//       that reaches them;
//   (4) silence until the deadline: no result yet; the telegram goes out again
//       with its 01 in the loop() that reads the next ENQ, and its answer is
//       delivered with retries 1;
//   (5) silence twice: TIMEOUT, retries 1;
//   (6) a lone ENQ during the wait counts as silence;
//   (7) part of an answer is not silence: TIMEOUT, no retry;
//   (8) an open frame met by silence is not sent again;
//   (9) the watchdog runs afresh from the retry: an ENQ 2.9 s after it still
//       carries the telegram, though the request is then 5.9 s old.
// The old timing, one pass later and no retry, is what the differential
// target runs the link on (kw_calls::kVs1Timing).
#include <cstdio>
#include <vector>

#include "fake_optolink.h"
#include "kw_calls.h"
#include "optolink_kw.h"

using namespace esphome::vitohome;
using Engine = optolink::KwExchange<FakeOptolink>;
using Bytes = std::vector<uint8_t>;
constexpr optolink::EnqLinkParams kParams = optolink::KW_LINK_PARAMS;

namespace {
int g_fail = 0;
int g_results = 0;
optolink::OptolinkResult g_status = optolink::OptolinkResult::CONTINUE;
uint8_t g_retries = 0;
Bytes g_data;

void check(bool ok, const char *what) {
  std::printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_fail;
}

// One loop() per ms of `ms`.
void run(Engine &kw, uint32_t ms) {
  for (uint32_t t = 0; t < ms; ++t) {
    kw.loop();
    optolink::optolink_test_clock_advance(1);
  }
}

bool ok_with(const Bytes &data, uint8_t retries) {
  return g_status == optolink::OptolinkResult::OK && g_data == data && g_retries == retries;
}
}  // namespace

int main() {
  optolink::optolink_test_clock_freeze();
  FakeOptolink uart;
  Engine kw(&uart);
  kw.on_result([](const optolink::Result &r) {
    ++g_results;
    g_status = r.status;
    g_retries = r.retries;
    g_data.assign(r.data, r.data != nullptr ? r.data + r.length : r.data);
  });
  kw.begin();
  const uint32_t deadline = kParams.answer_timeout_ms + 1;

  std::printf("== KW link timing: same pass, retry after silence ==\n");

  // --- (1) the ENQ answered in the loop() that reads it ----------------------
  check(kw_calls::read(kw, 0x00F8, 1), "(1) read accepted");
  uart.feed({0x05});
  kw.loop();
  check(uart.written() == Bytes({0x01, 0xF7, 0x00, 0xF8, 0x01}), "(1) 01 F7 00 F8 01 in the loop() that read the ENQ");
  uart.clear_written();
  uart.feed({0x20});
  kw.loop();
  check(g_results == 1 && ok_with({0x20}, 0), "(1) answered");

  // --- (2) chained from submit() ----------------------------------------------
  check(kw_calls::read(kw, 0x5525, 2), "(2) chained read accepted");
  check(uart.written() == Bytes({0xF7, 0x55, 0x25, 0x02}), "(2) F7 55 25 02 written by submit(), no loop()");
  uart.clear_written();
  uart.feed({0x4C, 0x00});
  kw.loop();
  check(g_results == 2 && ok_with({0x4C, 0x00}, 0), "(2) answered");

  // --- (3) a chained GFA read waits its pause -----------------------------------
  check(kw_calls::gfa_read(kw, 0x4050, 1), "(3) chained GFA read accepted");
  check(uart.written().empty(), "(3) nothing at once");
  optolink::optolink_test_clock_advance(optolink::KW_GFA_GAP_MS - 1);
  kw.loop();
  check(uart.written().empty(), "(3) nothing 1 ms before the pause ends");
  optolink::optolink_test_clock_advance(1);
  kw.loop();
  check(uart.written() == Bytes({0x6B, 0x40, 0x50, 0x01}), "(3) 6B 40 50 01 in the loop() that reaches 25 ms");
  uart.clear_written();
  uart.feed({0x24});
  kw.loop();
  check(g_results == 3 && ok_with({0x24}, 0), "(3) answered");

  // --- (4) silence, then the telegram again --------------------------------------
  optolink::optolink_test_clock_advance(kParams.chain_window_ms + 100);
  check(kw_calls::read(kw, 0x0886, 1), "(4) read accepted past the chain window");
  check(uart.written().empty(), "(4) it waits for an ENQ");
  uart.feed({0x05});
  kw.loop();
  check(uart.written() == Bytes({0x01, 0xF7, 0x08, 0x86, 0x01}), "(4) sent on the ENQ");
  uart.clear_written();
  run(kw, deadline + 1);
  check(g_results == 3, "(4) silence until the deadline: no result yet");
  uart.feed({0x05});
  kw.loop();
  check(uart.written() == Bytes({0x01, 0xF7, 0x08, 0x86, 0x01}),
        "(4) the telegram again, with its 01, on the next ENQ");
  uart.clear_written();
  uart.feed({0x33});
  kw.loop();
  check(g_results == 4 && ok_with({0x33}, 1), "(4) answered, retries 1");

  // --- (5) silence twice -----------------------------------------------------------
  optolink::optolink_test_clock_advance(kParams.chain_window_ms + 100);
  check(kw_calls::read(kw, 0x0888, 1), "(5) read accepted");
  uart.feed({0x05});
  kw.loop();
  run(kw, deadline + 1);
  uart.feed({0x05});
  kw.loop();
  uart.clear_written();
  run(kw, deadline + 1);
  check(g_results == 5 && g_status == optolink::OptolinkResult::TIMEOUT && g_retries == 1,
        "(5) silent again: TIMEOUT, retries 1");
  uart.feed({0x05});
  run(kw, 10);
  check(uart.written().empty(), "(5) and not sent a third time");

  // --- (6) a lone ENQ during the wait --------------------------------------------
  optolink::optolink_test_clock_advance(kParams.chain_window_ms + 100);
  check(kw_calls::read(kw, 0x088A, 2), "(6) read accepted");
  uart.feed({0x05});
  kw.loop();
  uart.clear_written();
  run(kw, 200);
  uart.feed({0x05});  // the device's next ENQ, while the answer is due
  run(kw, deadline + 1);
  check(g_results == 5, "(6) a lone ENQ, then nothing: no result yet");
  uart.feed({0x05});
  kw.loop();
  check(uart.written() == Bytes({0x01, 0xF7, 0x08, 0x8A, 0x02}), "(6) the telegram again on the next ENQ");
  uart.feed({0xE0, 0x3A});
  kw.loop();
  check(g_results == 6 && ok_with({0xE0, 0x3A}, 1), "(6) answered, retries 1");

  // --- (7) part of an answer -----------------------------------------------------
  optolink::optolink_test_clock_advance(kParams.chain_window_ms + 100);
  check(kw_calls::read(kw, 0x088C, 2), "(7) read accepted");
  uart.feed({0x05});
  kw.loop();
  uart.feed({0x11});
  run(kw, deadline + 1);
  check(g_results == 7 && g_status == optolink::OptolinkResult::TIMEOUT && g_retries == 0,
        "(7) one byte of two: TIMEOUT, no retry");

  // --- (8) an open frame -----------------------------------------------------------
  const Bytes frame = {0x01, 0xF7, 0x00, 0xF8, 0x01};
  check(kw_calls::frame(kw, frame.data(), static_cast<uint8_t>(frame.size()), 1), "(8) frame accepted");
  uart.feed({0x05});
  kw.loop();
  uart.clear_written();
  run(kw, deadline + 1);
  check(g_results == 8 && g_status == optolink::OptolinkResult::TIMEOUT && g_retries == 0,
        "(8) silence: TIMEOUT, no retry");
  uart.feed({0x05});
  run(kw, 10);
  check(uart.written().empty(), "(8) not sent again");

  // --- (9) the watchdog after a retry ---------------------------------------------
  optolink::optolink_test_clock_advance(kParams.chain_window_ms + 100);
  check(kw_calls::read(kw, 0x0890, 1), "(9) read accepted");
  run(kw, 2000);  // no ENQ for 2 s
  uart.feed({0x05});
  kw.loop();
  run(kw, deadline + 1);  // silence: the retry, 3 s after the submit
  run(kw, 2900);          // the next ENQ comes late
  check(g_results == 8, "(9) no watchdog TIMEOUT 5.9 s after the submit");
  uart.clear_written();
  uart.feed({0x05});
  kw.loop();
  check(uart.written() == Bytes({0x01, 0xF7, 0x08, 0x90, 0x01}), "(9) the telegram again on that ENQ");
  uart.feed({0x44});
  kw.loop();
  check(g_results == 9 && ok_with({0x44}, 1), "(9) answered, retries 1");

  std::printf("kw link timing: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
