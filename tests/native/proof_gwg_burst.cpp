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
//       bursting only ever removes a wait, it never adds one;
//   (9) a datapoint that never answers does NOT trip the fallback: a
//       burst-sent read that times out is retried once on a fresh ENQ, and
//       only an answer there counts as a burst failure (issue #35);
//  (10) bytes that arrive after the deadline are reported, not lost;
//  (11) the response deadline can be lengthened at runtime for a test.
//
// Built with -DVITOHOME_PROTOCOL_GWG by build_and_run_protocols.sh.
#include <cstdio>
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

// A confirmed burst failure: after a completed transaction, a burst-sent read
// of `addr` goes unanswered, and its retry on the next ENQ is answered.
static void burst_fails_enq_answers(Engine &a, FakeOptolink &u, uint8_t sync_addr, uint8_t addr) {
  sync_and_complete(a, u, sync_addr, 0x01);
  a.read(addr, 1);
  pump(a);
  u.clear_written();
  optolink::optolink_test_clock_advance(400);  // past RESPONSE_TIMEOUT_MS (250)
  pump(a);
  u.feed({0x05});
  a.loop();
  u.feed({0x01});
  pump(a);
  u.clear_written();
}

// A datapoint that does not answer: burst-sent and on the retry's ENQ alike.
static void burst_and_enq_silent(Engine &a, FakeOptolink &u, uint8_t sync_addr, uint8_t addr) {
  sync_and_complete(a, u, sync_addr, 0x01);
  a.read(addr, 1);
  pump(a);
  u.clear_written();
  optolink::optolink_test_clock_advance(400);
  pump(a);
  u.feed({0x05});
  a.loop();
  u.clear_written();
  optolink::optolink_test_clock_advance(400);
  pump(a);
}

int main() {
  // Before the engine is constructed, so its first _currentMillis sample and
  // every later one come from the same frozen clock.
  optolink::optolink_test_clock_freeze();

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
  // Burst-sent, so no ENQ_ACK in front (gwgBurstOmitsEnqAck, default true).
  const std::vector<uint8_t> want70 = {0xCB, 0x70, 0x01, 0x04};
  check(uart.written() == want70, "BURST: sent without waiting for an ENQ, no 0x01");
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
  optolink::optolink_test_clock_advance(optolink::GWGEngine::ENQ_VALIDITY_MS + 150);
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
  // next request must NOT ride the window -- it re-syncs. A burst-sent read is
  // first retried on a fresh ENQ (see (9)); the TIMEOUT comes when the retry
  // goes unanswered too.
  const int errors_before = g_errors;
  check(adapter.read(0x0073, 1), "read accepted (device will stay silent)");
  pump(adapter);
  check(!uart.written().empty(), "sent (riding the sync from the previous completion)");
  uart.clear_written();
  optolink::optolink_test_clock_advance(400);  // past RESPONSE_TIMEOUT_MS (250)
  pump(adapter);
  check(g_errors == errors_before, "burst-sent read unanswered -> no TIMEOUT yet");
  check(uart.written().empty(), "  the retry waits for an ENQ (the sync is gone)");
  uart.feed({0x05});
  adapter.loop();
  const std::vector<uint8_t> want73 = {0x01, 0xCB, 0x73, 0x01, 0x04};
  check(uart.written() == want73, "  and goes out on it, with the 0x01");
  uart.clear_written();
  optolink::optolink_test_clock_advance(400);
  pump(adapter);
  check(g_errors == errors_before + 1, "silent device -> TIMEOUT after the retry");
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
    const std::vector<uint8_t> want_write = {0xC8, 0x76, 0x01, 0x2A, 0x04};
    check(uart.written() == want_write, "BURST: write sent without waiting for an ENQ");
    uart.clear_written();
    uart.feed({0x00});  // GWG answers a write with a single ack byte
    pump(adapter);
    check(g_responses == before_write + 1, "write acked");

    // The read-back the hub issues from handle_write_response().
    check(adapter.read(0x0076, 1), "read-back accepted");
    pump(adapter);
    const std::vector<uint8_t> want_readback = {0xCB, 0x76, 0x01, 0x04};
    check(uart.written() == want_readback, "BURST: read-back rides the write's own sync");
    uart.clear_written();
    uart.feed({0x2A});
    pump(adapter);
    check(g_responses == before_write + 2, "read-back completes");
    check(g_errors == errors_before + 1, "no new errors across write + read-back");
  }

  // --- (5) repeated burst failures disable bursting ------------------------
  // BURST_FAILURES_BEFORE_FALLBACK consecutive burst-sent requests going
  // unanswered -- each one answered when retried on an ENQ -- means this unit
  // does not honour the reuse (the wiki may simply be right about it), so the
  // engine gives up on bursting for good.
  std::printf("  (driving %u consecutive burst failures ...)\n",
              static_cast<unsigned>(optolink::GWGEngine::BURST_FAILURES_BEFORE_FALLBACK));
  for (uint8_t i = 0; i < optolink::GWGEngine::BURST_FAILURES_BEFORE_FALLBACK; ++i)
    burst_fails_enq_answers(adapter, uart, static_cast<uint8_t>(0x50 + i), static_cast<uint8_t>(0x60 + i));
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

  // --- (7) the fallback notifies the hub, exactly once ---------------------
  // Until 2026-10-10 the fallback above happened in total silence: the
  // engine's optolink_log_w() is compiled out unless VITOHOME_DEBUG_OPTOLINK
  // is defined, so no logger level could surface it. A hardware capture hit
  // the fallback after two reads and the operator was never told the poll
  // ceiling had dropped. The callback is what the hub logs from, so the thing
  // worth pinning is that it fires -- and that it fires ONCE, not on every
  // subsequent timeout.
  //
  // Fresh engine: the one above has already latched _burstDisabled.
  {
    FakeOptolink u2;
    Engine a2(&u2);
    int fallbacks = 0;
    unsigned reported = 0;
    a2.onResponse([](const uint8_t *, uint8_t, uint16_t) {});
    a2.onError([](optolink::OptolinkResult, uint16_t) {});
    a2.onBurstFallback([&](uint8_t n) {
      fallbacks++;
      reported = n;
    });
    a2.begin();

    for (uint8_t i = 0; i < optolink::GWGEngine::BURST_FAILURES_BEFORE_FALLBACK; ++i)
      burst_fails_enq_answers(a2, u2, static_cast<uint8_t>(0x50 + i), static_cast<uint8_t>(0x60 + i));
    check(fallbacks == 1, "burst fallback notified the hub");
    check(reported == optolink::GWGEngine::BURST_FAILURES_BEFORE_FALLBACK, "  reported the failure count");

    // Drive more failures; _burstDisabled is latched, so no second callback.
    for (int i = 0; i < 3; ++i) {
      sync_and_complete(a2, u2, static_cast<uint8_t>(0x80 + i), 0x01);
      a2.read(static_cast<uint8_t>(0x90 + i), 1);
      pump(a2);
      optolink::optolink_test_clock_advance(400);
      pump(a2);
    }
    check(fallbacks == 1, "  and only once, however many follow");
  }

  // --- (8) gwgBurstOmitsEnqAck: the leading 0x01 on a burst-sent request ----
  // Frame byte 0 is ENQ_ACK. dannerph and uwen70 both treat it as the reply to
  // the device's 0x05 rather than part of the request, and both therefore omit
  // it when sending inside an already-open window. This project included it
  // unconditionally until the 2026-10-13 hardware A/B (see constants.h), which
  // made omit=true the default.
  //
  // Pinned here: the flag changes the burst frame and NOTHING else. An
  // ENQ-paced request keeps the 0x01 under both settings -- it is a genuine
  // acknowledgement there -- so flipping this can only ever affect a frame
  // that answers no ENQ.
  {
    const bool saved = optolink::gwgBurstOmitsEnqAck;
    for (const bool omit : {false, true}) {
      optolink::gwgBurstOmitsEnqAck = omit;

      FakeOptolink u3;
      Engine a3(&u3);
      a3.onResponse([](const uint8_t *, uint8_t, uint16_t) {});
      a3.onError([](optolink::OptolinkResult, uint16_t) {});
      a3.begin();

      // ENQ-paced: the ACK is real, so it is present either way.
      a3.read(0x006F, 1);
      u3.feed({0x05});
      a3.loop();
      const std::vector<uint8_t> enq_paced = {0x01, 0xCB, 0x6F, 0x01, 0x04};
      check(u3.written() == enq_paced,
            omit ? "omit=1: ENQ-paced read keeps the 0x01" : "omit=0: ENQ-paced read keeps the 0x01");
      u3.clear_written();
      u3.feed({0x26});
      pump(a3);

      // Burst-sent: no ENQ answered, so the 0x01 is dropped under omit=true.
      a3.read(0x0070, 1);
      pump(a3);
      const std::vector<uint8_t> with_ack = {0x01, 0xCB, 0x70, 0x01, 0x04};
      const std::vector<uint8_t> without = {0xCB, 0x70, 0x01, 0x04};
      check(u3.written() == (omit ? without : with_ack),
            omit ? "omit=1: burst read drops the 0x01" : "omit=0: burst read keeps the 0x01");
      u3.clear_written();
      u3.feed({0x5F});
      pump(a3);

      // And the same for a write, payload intact under both settings.
      const uint8_t payload[] = {0x2A};
      a3.write(0x0076, payload, 1);
      pump(a3);
      const std::vector<uint8_t> w_ack = {0x01, 0xC8, 0x76, 0x01, 0x2A, 0x04};
      const std::vector<uint8_t> w_no = {0xC8, 0x76, 0x01, 0x2A, 0x04};
      check(u3.written() == (omit ? w_no : w_ack), omit ? "omit=1: burst write drops the 0x01, payload intact"
                                                        : "omit=0: burst write keeps the 0x01, payload intact");
      u3.clear_written();
      u3.feed({0x00});
      pump(a3);
    }
    optolink::gwgBurstOmitsEnqAck = saved;
    check(optolink::gwgBurstOmitsEnqAck == true, "flag restored to its default (omit)");
  }

  // --- (9) a datapoint that never answers does not trip the fallback --------
  // Issue #35 (GWG_VBEM 0x2053 SW=0x1E): 54 consecutive `kmbus_eeprom`
  // entities went unanswered, and the first two of them switched bursting off
  // for the session although the unit bursts fine. A burst-sent read that times
  // out is now retried on a fresh ENQ; silent there as well, the datapoint is
  // what does not answer, and the burst counter is left alone.
  {
    FakeOptolink u9;
    Engine a9(&u9);
    int fallbacks = 0;
    int timeouts = 0;
    a9.onResponse([](const uint8_t *, uint8_t, uint16_t) {});
    a9.onError([&](optolink::OptolinkResult r, uint16_t) {
      if (r == optolink::OptolinkResult::TIMEOUT)
        timeouts++;
    });
    a9.onBurstFallback([&](uint8_t) { fallbacks++; });
    a9.begin();

    for (uint8_t i = 0; i < 5; ++i)
      burst_and_enq_silent(a9, u9, static_cast<uint8_t>(0x50 + i), 0x6E);
    check(timeouts == 5, "5 silent datapoints -> one TIMEOUT each, after the retry");
    check(fallbacks == 0, "  and no burst fallback: the ENQ-paced retries were silent too");

    sync_and_complete(a9, u9, 0x6F, 0x26);
    a9.read(0x0070, 1);
    pump(a9);
    const std::vector<uint8_t> burst70 = {0xCB, 0x70, 0x01, 0x04};
    check(u9.written() == burst70, "  bursting still on after them");
    u9.feed({0x5F});
    pump(a9);
  }

  // --- (10) bytes after the deadline are reported, not lost -----------------
  // The timeout branch drains the receive buffer, and INIT discards whatever
  // is not an ENQ -- so before this report a late answer left no trace, and a
  // TIMEOUT could not tell "too late" from "never".
  {
    FakeOptolink u10;
    Engine a10(&u10);
    std::vector<optolink::GWGEngine::LateAnswer> reports;
    int timeouts = 0;
    a10.onResponse([](const uint8_t *, uint8_t, uint16_t) {});
    a10.onError([&](optolink::OptolinkResult r, uint16_t) {
      if (r == optolink::OptolinkResult::TIMEOUT)
        timeouts++;
    });
    a10.onLateAnswer([&](const optolink::GWGEngine::LateAnswer &l) { reports.push_back(l); });
    a10.begin();

    // ENQ-paced (cold), so the TIMEOUT comes straight away, without a retry.
    a10.read(0x006E, 1);
    u10.feed({0x05});
    a10.loop();
    u10.clear_written();
    optolink::optolink_test_clock_advance(300);
    pump(a10);
    check(timeouts == 1, "ENQ-paced read unanswered -> TIMEOUT");
    optolink::optolink_test_clock_advance(300);
    u10.feed({0x3C});
    pump(a10);
    check(reports.empty(), "  the report stays open while more may come");
    optolink::optolink_test_clock_advance(optolink::GWGEngine::LATE_WATCH_MS);
    pump(a10);
    const bool one = reports.size() == 1;
    check(one, "  and is delivered once the watch lapses");
    if (one) {
      const auto &r = reports[0];
      check(r.address == 0x6E && r.received == 0 && r.expected == 1, "  address, nothing before the deadline, 1 due");
      check(r.late_count == 1 && r.late_stored == 1 && r.late[0] == 0x3C, "  the late byte itself");
      check(r.first_late_ms == 600, "  600 ms after the request: the deadline it needed");
      check(r.deadline_ms == 250 && !r.retried, "  against the 250 ms default, not a retry");
    }

    // Silent: a TIMEOUT and nothing else.
    a10.read(0x006F, 1);
    u10.feed({0x05});
    a10.loop();
    optolink::optolink_test_clock_advance(300);
    pump(a10);
    optolink::optolink_test_clock_advance(optolink::GWGEngine::LATE_WATCH_MS + 1);
    pump(a10);
    check(reports.size() == 1, "a silent datapoint: TIMEOUT only, no report");

    // Short: one of two bytes before the deadline.
    a10.read(0x0088, 2);
    u10.feed({0x05});
    a10.loop();
    u10.feed({0x01});
    pump(a10);
    optolink::optolink_test_clock_advance(300);
    pump(a10);
    optolink::optolink_test_clock_advance(optolink::GWGEngine::LATE_WATCH_MS + 1);
    pump(a10);
    check(reports.size() == 2 && reports[1].received == 1 && reports[1].expected == 2 && reports[1].late_count == 0,
          "a short answer: 1 of 2 bytes reported");

    // The next telegram closes the report: whatever follows its ENQ is its own.
    a10.read(0x006E, 1);
    u10.feed({0x05});
    a10.loop();
    u10.clear_written();
    optolink::optolink_test_clock_advance(300);
    pump(a10);
    a10.read(0x006F, 1);  // the hub queues the next poll at once
    optolink::optolink_test_clock_advance(200);
    u10.feed({0x3C, 0x05});
    pump(a10);
    check(reports.size() == 3 && reports[2].late_count == 1 && reports[2].first_late_ms == 500,
          "the next request's ENQ closes the report (late byte before it kept)");
    const std::vector<uint8_t> next6f = {0x01, 0xCB, 0x6F, 0x01, 0x04};
    check(u10.written() == next6f, "  and that request goes out on it");
    u10.feed({0x26});
    pump(a10);
  }

  // --- (11) the response deadline is a runtime variable ---------------------
  // set_gwg_response_timeout_ms() exists to find out how long a datapoint
  // needs (issue #35); the request watchdog has to grow with it, or the ENQ
  // wait it also covers would be cut short.
  {
    const uint32_t saved = optolink::gwgResponseTimeoutMs;
    FakeOptolink u11;
    Engine a11(&u11);
    int responses = 0;
    int errors = 0;
    uint8_t value = 0;
    a11.onResponse([&](const uint8_t *d, uint8_t, uint16_t) {
      responses++;
      value = d[0];
    });
    a11.onError([&](optolink::OptolinkResult, uint16_t) { errors++; });
    a11.begin();

    optolink::gwgResponseTimeoutMs = 1000;
    a11.read(0x006E, 1);
    u11.feed({0x05});
    a11.loop();
    optolink::optolink_test_clock_advance(600);
    pump(a11);
    check(errors == 0, "1000 ms deadline: no TIMEOUT 600 ms in");
    u11.feed({0x3C});
    pump(a11);
    check(responses == 1 && value == 0x3C, "  the 600 ms answer is delivered");

    optolink::optolink_test_clock_advance(optolink::GWGEngine::ENQ_VALIDITY_MS + 100);  // no burst
    pump(a11);  // read() stamps the request with the engine's last loop() time
    a11.read(0x006F, 1);
    pump(a11);
    optolink::optolink_test_clock_advance(optolink::GWGEngine::REQUEST_TIMEOUT_MS + 200);
    pump(a11);
    check(errors == 0, "  the request watchdog grew by the same 750 ms");
    u11.feed({0x05});
    a11.loop();
    u11.feed({0x26});
    pump(a11);
    check(responses == 2 && errors == 0, "  and the late ENQ still carries the read");

    optolink::gwgResponseTimeoutMs = saved;
    check(optolink::gwgResponseTimeoutMs == optolink::GWGEngine::RESPONSE_TIMEOUT_MS, "deadline restored (250 ms)");
  }

  std::printf("gwg burst: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
