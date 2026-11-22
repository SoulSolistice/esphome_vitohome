// The KW link's own timing: its two timing points, and a telegram met by
// silence sent once more on the device's next ENQ.
//
// A VScotHO1_72 left 17 of 6066 telegrams sent on an ENQ unanswered over 7.7 h
// (2026-11-21), hence the retry. Sent in the very pass that allowed it, it
// left twice as many unanswered, and chained ones too, most right after A305
// and A38F (2026-11-22), hence the pauses: the 01 20 ms after the ENQ, a
// chained telegram 25 ms after the answer.
//
// Pinned below, on a frozen clock:
//   (1) the 01 and the telegram go out in the first pass 20 ms after the ENQ,
//       not before, and the exchange wants a fast loop while it waits;
//   (2) a chained telegram goes out 25 ms after the answer, not from submit();
//   (3) a chained GFA read waits the same 25 ms, its own pause;
//   (4) ENQ_TO_SEND 0: the ENQ is answered in the pass that reads it;
//   (5) ANSWER_TO_NEXT 0: a chained telegram goes out from submit();
//   (6) the ENQ hold counts from the moment: a pass at 69 ms still answers,
//       one at 70 ms waits for the next ENQ;
//   (7) a value past the bound is clamped to it;
//   (8) silence until the deadline: no result yet, the telegram goes out again
//       with its 01 on the next ENQ, its answer delivered with retries 1;
//   (9) silence twice: TIMEOUT, retries 1;
//  (10) a lone ENQ during the wait counts as silence;
//  (11) part of an answer is not silence: TIMEOUT, no retry;
//  (12) an open frame met by silence is not sent again;
//  (13) the watchdog runs afresh from the retry: an ENQ 2.9 s after it still
//       carries the telegram, though the request is then about 6 s old.
// The differential target runs the link on VS1Engine's timing
// (kw_calls::kVs1Timing): one pass later, no pauses, no retry.
#include <cstdio>
#include <vector>

#include "fake_optolink.h"
#include "kw_calls.h"
#include "optolink_kw.h"

using namespace esphome::vitohome;
using Engine = optolink::KwExchange<FakeOptolink>;
using Bytes = std::vector<uint8_t>;
using optolink::TimingPoint;
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

void set(Engine &kw, TimingPoint point, uint32_t ms) { kw.set_timing(point, ms); }
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
  const uint32_t enq_to_send = kParams.enq_to_send_ms;
  const uint32_t deadline = kParams.answer_timeout_ms + 1;
  // Past the device's ENQ: the 01 is due, plus a pass.
  const uint32_t reply = enq_to_send + 1;

  std::printf("== KW link timing: pauses, retry after silence ==\n");
  check(kw.timing(TimingPoint::ENQ_TO_SEND) == 20 && kw.timing(TimingPoint::ANSWER_TO_NEXT) == 25,
        "the defaults: ENQ_TO_SEND 20 ms, ANSWER_TO_NEXT 25 ms");

  // --- (1) the ENQ answered 20 ms after it ------------------------------------
  check(kw_calls::read(kw, 0x00F8, 1), "(1) read accepted");
  uart.feed({0x05});
  run(kw, enq_to_send);
  check(uart.written().empty(), "(1) nothing in the 20 ms after the ENQ");
  check(kw.wants_fast_loop(), "(1) the exchange wants a fast loop meanwhile");
  kw.loop();
  check(uart.written() == Bytes({0x01, 0xF7, 0x00, 0xF8, 0x01}), "(1) 01 F7 00 F8 01 in the pass at 20 ms");
  check(!kw.wants_fast_loop(), "(1) no fast loop once it is out");
  uart.clear_written();
  uart.feed({0x20});
  kw.loop();
  check(g_results == 1 && ok_with({0x20}, 0), "(1) answered");

  // --- (2) chained 25 ms after the answer ----------------------------------------
  check(kw_calls::read(kw, 0x5525, 2), "(2) chained read accepted");
  check(uart.written().empty(), "(2) nothing from submit()");
  optolink::optolink_test_clock_advance(24);
  kw.loop();
  check(uart.written().empty(), "(2) nothing 24 ms after the answer");
  optolink::optolink_test_clock_advance(1);
  kw.loop();
  check(uart.written() == Bytes({0xF7, 0x55, 0x25, 0x02}), "(2) F7 55 25 02 in the pass at 25 ms");
  uart.clear_written();
  uart.feed({0x4C, 0x00});
  kw.loop();
  check(g_results == 2 && ok_with({0x4C, 0x00}, 0), "(2) answered");

  // --- (3) a chained GFA read --------------------------------------------------------
  static_assert(optolink::KW_GFA_GAP_MS == 25, "the GFA pause equals ANSWER_TO_NEXT here");
  check(kw_calls::gfa_read(kw, 0x4050, 1), "(3) chained GFA read accepted");
  optolink::optolink_test_clock_advance(24);
  kw.loop();
  check(uart.written().empty(), "(3) nothing 24 ms after the answer");
  optolink::optolink_test_clock_advance(1);
  kw.loop();
  check(uart.written() == Bytes({0x6B, 0x40, 0x50, 0x01}), "(3) 6B 40 50 01 at 25 ms");
  uart.clear_written();
  uart.feed({0x24});
  kw.loop();
  check(g_results == 3 && ok_with({0x24}, 0), "(3) answered");

  // --- (4) ENQ_TO_SEND 0: the same pass --------------------------------------------
  set(kw, TimingPoint::ENQ_TO_SEND, 0);
  optolink::optolink_test_clock_advance(kParams.chain_window_ms + 100);
  check(kw_calls::read(kw, 0x0810, 2), "(4) read accepted past the chain window");
  uart.feed({0x05});
  kw.loop();
  check(uart.written() == Bytes({0x01, 0xF7, 0x08, 0x10, 0x02}),
        "(4) ENQ_TO_SEND 0: sent in the pass that read the ENQ");
  uart.clear_written();
  uart.feed({0x5E, 0x01});
  kw.loop();
  check(g_results == 4 && ok_with({0x5E, 0x01}, 0), "(4) answered");

  // --- (5) ANSWER_TO_NEXT 0: from submit() -------------------------------------------
  set(kw, TimingPoint::ANSWER_TO_NEXT, 0);
  check(kw_calls::read(kw, 0x0816, 2), "(5) chained read accepted");
  check(uart.written() == Bytes({0xF7, 0x08, 0x16, 0x02}), "(5) ANSWER_TO_NEXT 0: written by submit()");
  uart.clear_written();
  uart.feed({0x5A, 0x01});
  kw.loop();
  check(g_results == 5 && ok_with({0x5A, 0x01}, 0), "(5) answered");
  set(kw, TimingPoint::ENQ_TO_SEND, enq_to_send);
  set(kw, TimingPoint::ANSWER_TO_NEXT, kParams.answer_to_next_ms);

  // --- (6) the ENQ hold counts from the moment -----------------------------------------
  optolink::optolink_test_clock_advance(kParams.chain_window_ms + 100);
  check(kw_calls::read(kw, 0x2900, 2), "(6) read accepted");
  uart.feed({0x05});
  kw.loop();  // reads the ENQ
  optolink::optolink_test_clock_advance(enq_to_send + kParams.enq_hold_ms - 1);
  kw.loop();
  check(uart.written() == Bytes({0x01, 0xF7, 0x29, 0x00, 0x02}), "(6) a pass 69 ms after the ENQ still answers it");
  uart.clear_written();
  uart.feed({0x5E, 0x01});
  kw.loop();
  optolink::optolink_test_clock_advance(kParams.chain_window_ms + 100);
  check(kw_calls::read(kw, 0x2544, 2), "(6) read accepted");
  uart.feed({0x05});
  kw.loop();
  optolink::optolink_test_clock_advance(enq_to_send + kParams.enq_hold_ms);
  kw.loop();
  check(uart.written().empty(), "(6) at 70 ms the ENQ is given up");
  uart.feed({0x05});
  run(kw, reply);
  check(uart.written() == Bytes({0x01, 0xF7, 0x25, 0x44, 0x02}), "(6) and the next ENQ carries it");
  uart.clear_written();
  uart.feed({0x64, 0x01});
  kw.loop();
  check(g_results == 7 && ok_with({0x64, 0x01}, 0), "(6) answered");

  // --- (7) the bounds ------------------------------------------------------------------
  uint32_t asked = 500;
  kw.set_timing(TimingPoint::ENQ_TO_SEND, asked);
  check(asked == optolink::ENQ_TO_SEND_MAX_MS && kw.timing(TimingPoint::ENQ_TO_SEND) == 100,
        "(7) ENQ_TO_SEND 500 is clamped to 100");
  asked = 9000;
  kw.set_timing(TimingPoint::ANSWER_TO_NEXT, asked);
  check(asked == optolink::ANSWER_TO_NEXT_MAX_MS && kw.timing(TimingPoint::ANSWER_TO_NEXT) == 400,
        "(7) ANSWER_TO_NEXT 9000 is clamped to 400");
  check(kw.timing_default(TimingPoint::ENQ_TO_SEND) == 20 && kw.timing_default(TimingPoint::ANSWER_TO_NEXT) == 25,
        "(7) the defaults are still known");
  set(kw, TimingPoint::ENQ_TO_SEND, enq_to_send);
  set(kw, TimingPoint::ANSWER_TO_NEXT, kParams.answer_to_next_ms);

  // --- (8) silence, then the telegram again --------------------------------------------
  optolink::optolink_test_clock_advance(kParams.chain_window_ms + 100);
  check(kw_calls::read(kw, 0x0886, 1), "(8) read accepted");
  uart.feed({0x05});
  run(kw, reply);
  check(uart.written() == Bytes({0x01, 0xF7, 0x08, 0x86, 0x01}), "(8) sent on the ENQ");
  uart.clear_written();
  run(kw, deadline + 1);
  check(g_results == 7, "(8) silence until the deadline: no result yet");
  uart.feed({0x05});
  run(kw, reply);
  check(uart.written() == Bytes({0x01, 0xF7, 0x08, 0x86, 0x01}),
        "(8) the telegram again, with its 01, on the next ENQ");
  uart.clear_written();
  uart.feed({0x33});
  kw.loop();
  check(g_results == 8 && ok_with({0x33}, 1), "(8) answered, retries 1");

  // --- (9) silence twice -----------------------------------------------------------------
  optolink::optolink_test_clock_advance(kParams.chain_window_ms + 100);
  check(kw_calls::read(kw, 0x0888, 1), "(9) read accepted");
  uart.feed({0x05});
  run(kw, reply);
  run(kw, deadline + 1);
  uart.feed({0x05});
  run(kw, reply);
  uart.clear_written();
  run(kw, deadline + 1);
  check(g_results == 9 && g_status == optolink::OptolinkResult::TIMEOUT && g_retries == 1,
        "(9) silent again: TIMEOUT, retries 1");
  uart.feed({0x05});
  run(kw, reply + 10);
  check(uart.written().empty(), "(9) and not sent a third time");

  // --- (10) a lone ENQ during the wait -------------------------------------------------
  optolink::optolink_test_clock_advance(kParams.chain_window_ms + 100);
  check(kw_calls::read(kw, 0x088A, 2), "(10) read accepted");
  uart.feed({0x05});
  run(kw, reply);
  uart.clear_written();
  run(kw, 200);
  uart.feed({0x05});  // the device's next ENQ, while the answer is due
  run(kw, deadline + 1);
  check(g_results == 9, "(10) a lone ENQ, then nothing: no result yet");
  uart.feed({0x05});
  run(kw, reply);
  check(uart.written() == Bytes({0x01, 0xF7, 0x08, 0x8A, 0x02}), "(10) the telegram again on the next ENQ");
  uart.feed({0xE0, 0x3A});
  kw.loop();
  check(g_results == 10 && ok_with({0xE0, 0x3A}, 1), "(10) answered, retries 1");

  // --- (11) part of an answer -------------------------------------------------------------
  optolink::optolink_test_clock_advance(kParams.chain_window_ms + 100);
  check(kw_calls::read(kw, 0x088C, 2), "(11) read accepted");
  uart.feed({0x05});
  run(kw, reply);
  uart.feed({0x11});
  run(kw, deadline + 1);
  check(g_results == 11 && g_status == optolink::OptolinkResult::TIMEOUT && g_retries == 0,
        "(11) one byte of two: TIMEOUT, no retry");

  // --- (12) an open frame -------------------------------------------------------------------
  const Bytes frame = {0x01, 0xF7, 0x00, 0xF8, 0x01};
  check(kw_calls::frame(kw, frame.data(), static_cast<uint8_t>(frame.size()), 1), "(12) frame accepted");
  uart.feed({0x05});
  run(kw, reply);
  uart.clear_written();
  run(kw, deadline + 1);
  check(g_results == 12 && g_status == optolink::OptolinkResult::TIMEOUT && g_retries == 0,
        "(12) silence: TIMEOUT, no retry");
  uart.feed({0x05});
  run(kw, reply + 10);
  check(uart.written().empty(), "(12) not sent again");

  // --- (13) the watchdog after a retry -------------------------------------------------------
  optolink::optolink_test_clock_advance(kParams.chain_window_ms + 100);
  check(kw_calls::read(kw, 0x0890, 1), "(13) read accepted");
  run(kw, 2000);  // no ENQ for 2 s
  uart.feed({0x05});
  run(kw, reply);
  run(kw, deadline + 1);  // silence: the retry, 3 s after the submit
  run(kw, 2900);          // the next ENQ comes late
  check(g_results == 12, "(13) no watchdog TIMEOUT about 6 s after the submit");
  uart.clear_written();
  uart.feed({0x05});
  run(kw, reply);
  check(uart.written() == Bytes({0x01, 0xF7, 0x08, 0x90, 0x01}), "(13) the telegram again on that ENQ");
  uart.feed({0x44});
  kw.loop();
  check(g_results == 13 && ok_with({0x44}, 1), "(13) answered, retries 1");

  std::printf("kw link timing: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
