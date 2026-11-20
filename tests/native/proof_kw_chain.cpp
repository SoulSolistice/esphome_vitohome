// KW chaining and its two windows (THIRD_PARTY.md #22), on the KW exchange
// (optolink_kw.h).
//
// A VScotHO1_72 (0x20CB) answers a whole poll cycle off one ENQ: on a capture
// of 2026-08-24, nine chains with a median of 29 telegrams per ENQ and at most
// 49. Only the first telegram of a chain carries the 0x01. The ENQ must be
// answered within the ENQ hold (50 ms); a chain may pause for the chain window
// (1500 ms, vogod's figure). The capture measured 22-42 ms from an answer to
// the next request.
//
// Pinned below:
//   (1) the first telegram after an ENQ carries the 0x01 prefix;
//   (2) chained telegrams carry no prefix and need no ENQ;
//   (3) a chain survives a pause longer than the ENQ hold;
//   (4) past the chain window the link waits for a real ENQ again;
//   (5) a byte arriving between the answer and the next request ends the
//       chain, so the request does not go out with that byte buffered where
//       it would be read as the answer;
//   (6) begin() restarts the wait for an ENQ, so no EOT goes out at once.
//
// Time is injected (optolink_test_clock_freeze in optolink_clock.h), so the
// 200 ms pause of (3) is pinned on both sides: past the ENQ hold and inside
// the chain window.
#include <cstdio>
#include <vector>

#include "fake_optolink.h"
#include "kw_calls.h"
#include "optolink_kw.h"

using namespace esphome::vitohome;
using Engine = optolink::KwExchange<FakeOptolink>;
constexpr optolink::EnqLinkParams kParams = optolink::KW_LINK_PARAMS;

namespace {
int g_responses = 0;
int g_errors = 0;
uint8_t g_last_payload = 0;
}  // namespace

static void pump(Engine &kw, int n = 8) {
  for (int i = 0; i < n; ++i)
    kw.loop();
}

int main() {
  // Before the engine is constructed, so every clock sample it takes comes
  // from the same frozen clock.
  optolink::optolink_test_clock_freeze();

  FakeOptolink uart;
  Engine kw(&uart);
  kw.on_result([](const optolink::Result &r) {
    if (r.status != optolink::OptolinkResult::OK) {
      g_errors++;
      return;
    }
    g_responses++;
    if (r.data != nullptr && r.length > 0)
      g_last_payload = r.data[0];
  });
  kw.begin();

  int failures = 0;
  auto check = [&failures](bool ok, const char *what) {
    std::printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
      failures++;
  };

  // --- (1) first telegram of a chain: ENQ -> 0x01 + telegram ----------------
  check(kw_calls::read(kw, 0x00F8, 1), "first read accepted");
  uart.feed({0x05});
  pump(kw);
  const std::vector<uint8_t> want_first = {0x01, 0xF7, 0x00, 0xF8, 0x01};
  check(uart.written() == want_first, "first telegram carries the 0x01 prefix");
  uart.clear_written();
  uart.feed({0x20});
  pump(kw);
  check(g_responses == 1 && g_last_payload == 0x20, "first read completes");

  // --- (2) chained telegrams: no prefix, no ENQ ----------------------------
  // The capture's shape: 01:F7:00:F8:01 then F7:00:F9:01, F7:00:FA:01, ...
  // with no 0x01 and no 0x05 in between.
  const uint8_t chain[] = {0xF9, 0xFA, 0xFB};
  const uint8_t vals[] = {0xCB, 0x03, 0x51};  // the VScotHO1_72's ident tail
  for (int i = 0; i < 3; ++i) {
    check(kw_calls::read(kw, static_cast<uint16_t>(0x0000 + chain[i]), 1), "chained read accepted");
    pump(kw);
    const std::vector<uint8_t> want = {0xF7, 0x00, chain[i], 0x01};
    check(uart.written() == want, "  chained telegram: no ENQ, no 0x01 prefix");
    uart.clear_written();
    uart.feed({vals[i]});
    pump(kw);
  }
  check(g_responses == 4 && g_errors == 0, "whole chain completed off one ENQ");

  // --- (3) a pause longer than the ENQ hold --------------------------------
  static_assert(200u > kParams.enq_hold_ms, "the pause must exceed the ENQ hold");
  static_assert(200u < kParams.chain_window_ms, "the pause must stay inside the chain window");
  optolink::optolink_test_clock_advance(200);
  check(kw_calls::read(kw, 0x0025, 1), "read accepted after a 200 ms pause");
  pump(kw);
  const std::vector<uint8_t> want_after_pause = {0xF7, 0x00, 0x25, 0x01};
  check(uart.written() == want_after_pause, "chain survives the pause (no ENQ needed)");
  uart.clear_written();
  uart.feed({0x74});
  pump(kw);
  check(g_responses == 5, "  and completes");

  // --- (4) past the chain window the link re-syncs -------------------------
  optolink::optolink_test_clock_advance(kParams.chain_window_ms + 150);
  check(kw_calls::read(kw, 0x0026, 1), "read accepted after the window lapsed");
  pump(kw);
  check(uart.written().empty(), "expired window -> nothing sent without an ENQ");
  uart.feed({0x05});
  pump(kw);
  const std::vector<uint8_t> want_resync = {0x01, 0xF7, 0x00, 0x26, 0x01};
  check(uart.written() == want_resync, "re-synced: 0x01 prefix is back");
  uart.clear_written();
  uart.feed({0x32});
  pump(kw);
  check(g_responses == 6, "  and completes");

  // --- (5) a byte between answer and request ends the chain ----------------
  // On an idle KW bus that byte is the device's next ENQ.
  const int before_guard = g_responses;
  uart.feed({0x05});
  check(kw_calls::read(kw, 0x0027, 1), "read accepted (byte pending)");
  pump(kw);
  const std::vector<uint8_t> want_guarded = {0x01, 0xF7, 0x00, 0x27, 0x01};
  check(uart.written() == want_guarded, "pending byte taken as a sync, prefix re-sent");
  uart.clear_written();
  uart.feed({0x99});  // the answer, distinguishable from the 0x05
  pump(kw);
  check(g_responses == before_guard + 1, "  read completed");
  check(g_last_payload == 0x99, "  payload is the data byte, not the buffered 0x05");
  check(g_errors == 0, "  no errors anywhere in the run");

  // --- (6) begin() restarts the wait for an ENQ ----------------------------
  // The hub calls begin() after a timed frame, which held the port longer than
  // the EOT interval.
  uart.clear_written();
  optolink::optolink_test_clock_advance(kParams.eot_interval_ms + 500);
  kw.begin();
  pump(kw);
  check(uart.written().empty(), "begin(): no EOT right after a restart");
  optolink::optolink_test_clock_advance(kParams.eot_interval_ms + 1);
  pump(kw);
  check(uart.written() == std::vector<uint8_t>{0x04}, "  the EOT once no ENQ came for the EOT interval");

  std::printf("kw chaining: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
