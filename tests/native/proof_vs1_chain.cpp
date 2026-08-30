// VS1/KW chaining, and the split of the two sync windows (THIRD_PARTY.md #22).
//
// VS1 has always chained telegrams onto one sync -- _syncRecv() re-sends
// without waiting for a fresh ENQ -- and that is HARDWARE-CONFIRMED: a
// 2026-08-24 capture from a VScotHO1_72 (0x20CB) shows nine ENQ-delimited
// chains with a median of 29 telegrams per ENQ and a maximum of 49, a whole
// 60 s poll cycle answered off a single sync in ~2.15 s. The 0x01 (ENQ_ACK)
// prefix appears on the first telegram of each chain and on no other, which is
// exactly the _syncEnq()/_syncRecv() split.
//
// What changed: one 50 ms SYNC_WINDOW_MS used to serve both
//   _syncEnq()  -- how fast we must answer the device's ENQ (protocol), and
//   _syncRecv() -- how long a chain may pause (throughput),
// which pinned the second to the first. The same capture measures the second
// directly: answer-to-next-request ran 22-42 ms over 235 transactions, i.e.
// ~8 ms under the old ceiling. They are now ENQ_ACK_WINDOW_MS (50, unchanged)
// and CHAIN_WINDOW_MS (1500, vogod's figure for the same job).
//
// Pinned below:
//   (1) the first telegram after an ENQ carries the 0x01 prefix;
//   (2) chained telegrams carry no prefix and need no ENQ;
//   (3) a chain survives a pause the OLD 50 ms window would have ended;
//   (4) past CHAIN_WINDOW_MS the engine waits for a real ENQ again;
//   (5) THE GUARD: a byte arriving between the answer and the next request
//       ends the chain, so the request cannot go out with that byte buffered
//       where _receive() would read it as its payload.
//
// Built with -DVITOHOME_PROTOCOL_KW by build_and_run_protocols.sh.
//
// TIME IS INJECTED, NOT SLEPT (see optolink_test_clock_freeze in helpers.h).
// Case (3) is the one THIRD_PARTY.md item 23 named as the remaining latent
// flake, and it is the two-sided kind: its 200 ms pause must land past the old
// 50 ms ENQ_ACK_WINDOW_MS *and* inside the 1500 ms CHAIN_WINDOW_MS. sleep_for
// guarantees only "at least 200 ms", so the upper bound was never actually
// pinned -- a host stall past 1500 ms expired the window and failed case (3)
// for a reason having nothing to do with the engine. Advancing a frozen clock
// pins both sides exactly; the two static_asserts below keep the constants
// from drifting out from under the number.
#include <cstdio>
#include <vector>

#include "fake_optolink.h"
#include "protocol_select.h"

using namespace esphome::vitohome;
using Engine = optolink::OptolinkEngine<SelectedProtocol>;

namespace {
int g_responses = 0;
int g_errors = 0;
uint8_t g_last_payload = 0;
}  // namespace

static void pump(Engine &a, int n = 8) {
  for (int i = 0; i < n; ++i)
    a.loop();
}

int main() {
  // Before the engine is constructed, so its first _currentMillis sample and
  // every later one come from the same frozen clock.
  optolink::optolink_test_clock_freeze();

  FakeOptolink uart;
  Engine adapter(&uart);
  adapter.onResponse([](const uint8_t *d, uint8_t len, uint16_t) {
    g_responses++;
    if (d != nullptr && len > 0)
      g_last_payload = d[0];
  });
  adapter.onError([](optolink::OptolinkResult, uint16_t) { g_errors++; });
  adapter.begin();

  int failures = 0;
  auto check = [&failures](bool ok, const char *what) {
    std::printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
      failures++;
  };

  // --- (1) first telegram of a chain: ENQ -> ENQ_ACK + frame ---------------
  check(adapter.read(0x00F8, 1), "first read accepted");
  uart.feed({0x05});
  pump(adapter);
  const std::vector<uint8_t> want_first = {0x01, 0xF7, 0x00, 0xF8, 0x01};
  check(uart.written() == want_first, "first telegram carries the 0x01 ENQ_ACK prefix");
  uart.clear_written();
  uart.feed({0x20});
  pump(adapter);
  check(g_responses == 1 && g_last_payload == 0x20, "first read completes");

  // --- (2) chained telegrams: no prefix, no ENQ ----------------------------
  // This is the shape the hardware capture shows: 01:F7:00:F8:01 then
  // F7:00:F9:01, F7:00:FA:01, ... with no 0x01 and no intervening 0x05.
  const uint8_t chain[] = {0xF9, 0xFA, 0xFB};
  const uint8_t vals[] = {0xCB, 0x03, 0x51};  // the real VScotHO1_72 ident tail
  for (int i = 0; i < 3; ++i) {
    check(adapter.read(static_cast<uint16_t>(0x0000 + chain[i]), 1), "chained read accepted");
    pump(adapter);
    const std::vector<uint8_t> want = {0xF7, 0x00, chain[i], 0x01};
    check(uart.written() == want, "  chained telegram: no ENQ, no 0x01 prefix");
    uart.clear_written();
    uart.feed({vals[i]});
    pump(adapter);
  }
  check(g_responses == 4 && g_errors == 0, "whole chain completed off one ENQ");

  // --- (3) a pause the OLD 50 ms window would have killed ------------------
  // The two-sided case; see the header note. Both bounds are asserted here so
  // re-tuning either window breaks the build rather than the meaning of 200.
  static_assert(200u > optolink::VS1Engine::ENQ_ACK_WINDOW_MS, "pause must exceed the old sync window");
  static_assert(200u < optolink::VS1Engine::CHAIN_WINDOW_MS, "pause must stay inside the chain window");
  optolink::optolink_test_clock_advance(200);
  check(adapter.read(0x0025, 1), "read accepted after a 200 ms pause");
  pump(adapter);
  const std::vector<uint8_t> want_after_pause = {0xF7, 0x00, 0x25, 0x01};
  check(uart.written() == want_after_pause, "chain SURVIVES the pause (no ENQ needed)");
  uart.clear_written();
  uart.feed({0x74});
  pump(adapter);
  check(g_responses == 5, "  and completes");

  // --- (4) past CHAIN_WINDOW_MS the engine re-syncs -------------------------
  optolink::optolink_test_clock_advance(optolink::VS1Engine::CHAIN_WINDOW_MS + 150);
  check(adapter.read(0x0026, 1), "read accepted after the window lapsed");
  pump(adapter);
  check(uart.written().empty(), "expired window -> nothing sent without an ENQ");
  uart.feed({0x05});
  pump(adapter);
  const std::vector<uint8_t> want_resync = {0x01, 0xF7, 0x00, 0x26, 0x01};
  check(uart.written() == want_resync, "re-synced: 0x01 prefix is back");
  uart.clear_written();
  uart.feed({0x32});
  pump(adapter);
  check(g_responses == 6, "  and completes");

  // --- (5) THE GUARD that makes a long window safe -------------------------
  // A byte arrives between the answer and the next request. On an idle KW bus
  // that is the device's next ENQ. If the chain ignored it, the next request
  // would go out with that byte still buffered and _receive() would read it as
  // the payload -- a misread. _syncRecv() must hand control back to INIT.
  const int before_guard = g_responses;
  uart.feed({0x05});  // device speaks between transactions
  check(adapter.read(0x0027, 1), "read accepted (byte pending)");
  pump(adapter);
  const std::vector<uint8_t> want_guarded = {0x01, 0xF7, 0x00, 0x27, 0x01};
  check(uart.written() == want_guarded, "GUARD: pending byte consumed as a sync, prefix re-sent");
  uart.clear_written();
  uart.feed({0x99});  // the real payload, distinguishable from the 0x05
  pump(adapter);
  check(g_responses == before_guard + 1, "  read completed");
  check(g_last_payload == 0x99, "  payload is the data byte, NOT the 0x05 that was buffered");
  check(g_errors == 0, "  no errors anywhere in the run");

  std::printf("vs1 chaining: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
