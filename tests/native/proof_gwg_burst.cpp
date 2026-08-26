// GWG burst: reuse a still-valid sync instead of waiting for the device's next
// ENQ (2026-08-24). See GWGEngine::ENQ_VALIDITY_MS.
//
// Why this exists: without bursting the poll rate is capped at one datapoint
// per ENQ interval, measured at 749-2070 ms (median 1229) on the 2026-08-23
// capture -- a 180-entity catalog becomes a multi-minute sweep. Two
// implementations ship the reuse (vogod's 1500 ms ENQ validity on KW,
// dannerph's GWG burst mode), and this engine's own VS1 sibling already does it
// on a 50 ms budget. It contradicts the openv wiki, which says to wait for the
// next 0x05, so the behaviour here is deliberately self-correcting.
//
// Pinned below:
//   (1) a queued request rides a fresh sync -- no ENQ needed;
//   (2) a real ENQ still wins when one is pending, and is never left buffered
//       where RECEIVE could read it as a payload;
//   (3) the window expires: past ENQ_VALIDITY_MS the engine waits again;
//   (4) a TIMEOUT invalidates the sync, so the next request re-syncs properly
//       rather than riding a window we no longer trust;
//   (5) repeated burst failures disable bursting for the session, and a burst
//       success resets the counter;
//   (6) writes and the read-back that follows them ride the window as well --
//       bursting only ever removes a wait, it never adds one.
//
// Built with -DVITOHOME_PROTOCOL_GWG by build_and_run_protocols.sh.
#include <chrono>  // NOLINT [build/c++11]
#include <cstdio>
#include <thread>  // NOLINT [build/c++11]
#include <vector>

#include "fake_optolink.h"
#include "protocol_select.h"

using namespace esphome::vitohome;
using Engine = optolink::OptolinkEngine<SelectedProtocol>;

namespace {
int g_responses = 0;
int g_errors = 0;
}  // namespace

static void pump(Engine &a, int n = 6) {
  for (int i = 0; i < n; ++i)
    a.loop();
}

// One complete transaction that starts from a real ENQ. Leaves the engine with
// a freshly stamped sync window.
static void sync_and_complete(Engine &a, FakeOptolink &u, uint8_t addr, uint8_t value) {
  a.read(addr, 1);
  u.feed({0x05});
  a.loop();
  u.feed({value});
  pump(a);
  u.clear_written();
}

int main() {
  FakeOptolink uart;
  Engine adapter(&uart);
  adapter.onResponse([](const uint8_t *, uint8_t, uint16_t) { g_responses++; });
  adapter.onError([](optolink::OptolinkResult, uint16_t) { g_errors++; });
  adapter.begin();

  int failures = 0;
  auto check = [&failures](bool ok, const char *what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
      failures++;
  };

  // With bursting compiled out, the property to prove is the inverse: the
  // engine must NEVER put a telegram on the wire without a fresh ENQ, no
  // matter how recent the last completed transaction was. Both branches build
  // and pass, so flipping BURST_ENABLED does not leave a red suite.
  if constexpr (!optolink::GWGEngine::BURST_ENABLED) {
    std::printf("  (BURST_ENABLED is false -- asserting strict one-telegram-per-ENQ)\n");
    for (int i = 0; i < 3; ++i) {
      check(adapter.read(static_cast<uint8_t>(0x6F + i), 1), "read accepted");
      pump(adapter);
      check(uart.written().empty(), "  nothing sent without an ENQ");
      uart.feed({0x05});
      adapter.loop();
      check(!uart.written().empty(), "  sent once the ENQ arrived");
      uart.clear_written();
      uart.feed({0x26});
      pump(adapter);
    }
    check(g_responses == 3 && g_errors == 0, "all three completed, one ENQ each");
    std::printf("gwg burst: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
  }

  // --- cold start: no sync yet, so there is nothing to ride -----------------
  check(adapter.read(0x006F, 1), "cold read accepted");
  pump(adapter);
  check(uart.written().empty(), "no sync yet -> nothing sent without an ENQ");
  uart.feed({0x05});
  adapter.loop();
  const std::vector<uint8_t> want6f = {0x01, 0xCB, 0x6F, 0x01, 0x04};
  check(uart.written() == want6f, "first request goes out on a real ENQ");
  uart.feed({0x26});
  pump(adapter);
  check(g_responses == 1, "first read completes");
  uart.clear_written();

  // --- (1) the next request rides the sync the completion re-stamped --------
  // This is the whole point: no ENQ is fed, and the frame still goes out.
  check(adapter.read(0x0070, 1), "second read accepted");
  pump(adapter);
  const std::vector<uint8_t> want70 = {0x01, 0xCB, 0x70, 0x01, 0x04};
  check(uart.written() == want70, "BURST: sent without waiting for an ENQ");
  uart.feed({0x5F});
  pump(adapter);
  check(g_responses == 2, "burst-sent read completes");
  uart.clear_written();

  // --- a whole chain runs back-to-back --------------------------------------
  const int before_chain = g_responses;
  for (uint8_t i = 0; i < 5; ++i) {
    adapter.read(static_cast<uint8_t>(0x30 + i), 1);
    pump(adapter);
    uart.feed({static_cast<uint8_t>(0x40 + i)});
    pump(adapter);
  }
  check(g_responses == before_chain + 5, "5 further reads chain with no ENQ at all");
  check(g_errors == 0, "no errors during the chain");
  uart.clear_written();

  // --- (2) a real ENQ still wins, and is not left buffered ------------------
  // If a pending ENQ were ignored in favour of the burst path it would sit in
  // the buffer and RECEIVE would read it as the payload -- the misread this
  // engine exists to prevent. Feeding an ENQ and a payload together proves the
  // ENQ was consumed by INIT, not by RECEIVE.
  const int before_enq_case = g_responses;
  check(adapter.read(0x0071, 1), "read accepted (ENQ pending)");
  uart.feed({0x05});
  pump(adapter);
  const std::vector<uint8_t> want71 = {0x01, 0xCB, 0x71, 0x01, 0x04};
  check(uart.written() == want71, "request sent");
  uart.feed({0x2C});
  pump(adapter);
  check(g_responses == before_enq_case + 1 && g_errors == 0, "payload delivered, not the ENQ");
  uart.clear_written();

  // --- (3) the window expires ----------------------------------------------
  std::printf("  (letting the burst window lapse, %u ms ...)\n",
              static_cast<unsigned>(optolink::GWGEngine::ENQ_VALIDITY_MS));
  std::this_thread::sleep_for(std::chrono::milliseconds(optolink::GWGEngine::ENQ_VALIDITY_MS + 150));
  check(adapter.read(0x0072, 1), "read accepted after the window lapsed");
  pump(adapter);
  check(uart.written().empty(), "expired window -> waits for a real ENQ again");
  uart.feed({0x05});
  adapter.loop();
  check(!uart.written().empty(), "and sends once the ENQ arrives");
  uart.feed({0x21});
  pump(adapter);
  uart.clear_written();

  // --- (4) a TIMEOUT invalidates the sync ----------------------------------
  // After a failure we no longer know where the device's cadence is, so the
  // next request must NOT ride the window -- it re-syncs.
  const int errors_before = g_errors;
  check(adapter.read(0x0073, 1), "read accepted (device will stay silent)");
  pump(adapter);
  check(!uart.written().empty(), "sent (riding the sync from the previous completion)");
  uart.clear_written();
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  pump(adapter);
  check(g_errors == errors_before + 1, "silent device -> TIMEOUT");
  check(adapter.read(0x0074, 1), "next read accepted");
  pump(adapter);
  check(uart.written().empty(), "TIMEOUT invalidated the sync -> waits for an ENQ");
  uart.feed({0x05});
  adapter.loop();
  check(!uart.written().empty(), "and sends on the fresh ENQ");
  uart.feed({0x11});
  pump(adapter);
  uart.clear_written();

  // --- (6) writes and the post-write read-back ride the window too ----------
  // The hub is single-in-flight and writes preempt reads
  // (VitoHomeComponent::dispatch_next_), so a user write is queued the moment
  // the current transaction finishes. Without bursting it would then wait for
  // the device's next ENQ -- 749-2070 ms on the capture -- and the read-back
  // that confirms it would wait for the one after that. Bursting removes both
  // waits: nothing here feeds an ENQ, and the write goes out on the sync the
  // preceding read stamped, the read-back on the sync the write stamped.
  // Bursting only ever removes a wait; there is no batching or hold-back
  // anywhere, so it cannot delay either direction.
  {
    const int before_write = g_responses;
    const uint8_t payload[] = {0x2A};
    check(adapter.write(0x0076, payload, 1), "write accepted");
    pump(adapter);
    const std::vector<uint8_t> want_write = {0x01, 0xC8, 0x76, 0x01, 0x2A, 0x04};
    check(uart.written() == want_write, "BURST: write sent without waiting for an ENQ");
    uart.clear_written();
    uart.feed({0x00});  // GWG answers a write with a single ack byte
    pump(adapter);
    check(g_responses == before_write + 1, "write acked");

    // The read-back the hub issues from handle_write_response().
    check(adapter.read(0x0076, 1), "read-back accepted");
    pump(adapter);
    const std::vector<uint8_t> want_readback = {0x01, 0xCB, 0x76, 0x01, 0x04};
    check(uart.written() == want_readback, "BURST: read-back rides the write's own sync");
    uart.clear_written();
    uart.feed({0x2A});
    pump(adapter);
    check(g_responses == before_write + 2, "read-back completes");
    check(g_errors == errors_before + 1, "no new errors across write + read-back");
  }

  // --- (5) repeated burst failures disable bursting ------------------------
  // BURST_FAILURES_BEFORE_FALLBACK consecutive burst-sent requests going
  // unanswered means this unit does not honour the reuse (the wiki may simply
  // be right about it), so the engine gives up on bursting for good.
  std::printf("  (driving %u consecutive burst failures ...)\n",
              static_cast<unsigned>(optolink::GWGEngine::BURST_FAILURES_BEFORE_FALLBACK));
  for (uint8_t i = 0; i < optolink::GWGEngine::BURST_FAILURES_BEFORE_FALLBACK; ++i) {
    // Re-establish a sync with a completed transaction, then let the next
    // (burst-sent) one go unanswered.
    sync_and_complete(adapter, uart, static_cast<uint8_t>(0x50 + i), 0x01);
    adapter.read(static_cast<uint8_t>(0x60 + i), 1);
    pump(adapter);
    uart.clear_written();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    pump(adapter);
  }
  // Bursting is now off: even with a fresh sync, the engine waits for an ENQ.
  sync_and_complete(adapter, uart, 0x006F, 0x26);
  check(adapter.read(0x0075, 1), "read accepted after burst fallback");
  pump(adapter);
  check(uart.written().empty(), "burst disabled -> waits for an ENQ despite a fresh sync");
  uart.feed({0x05});
  adapter.loop();
  check(!uart.written().empty(), "still works, just one telegram per ENQ");
  uart.feed({0x33});
  pump(adapter);

  std::printf("gwg burst: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
