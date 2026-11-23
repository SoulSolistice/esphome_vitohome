// The GWG exchange (optolink_gwg.h) through its own interface, Request in and
// Result out, on a frozen clock. The differential target compares its bytes
// and callbacks with the reference GWGEngine's; these cases pin what only the
// Result and the settings say.
//   A) a read goes out on the ENQ, in the pass that reads it; the Result
//      carries the answer and its timing;
//   B) a read within GWG_BURST_WINDOW_MS of an answer goes out at once,
//      without its 01; one past it waits for the ENQ;
//   C) no ENQ for the watchdog: TIMEOUT, never_sent; sent and unanswered:
//      TIMEOUT, not never_sent;
//   D) an EEPROM run, one cell per telegram: FF FF is FF with
//      unserved_eeprom; a pair that does not sum to FF is CRC;
//   E) refusals: the P300 and KW lanes, a second request while busy, one
//      from inside the result callback, a run past 0xFF;
//   F) a two-byte EEPROM write on the GWG lane is the pair as given;
//   G) a burst read met by silence goes out again on the ENQ, reported as a
//      retried late answer; two such retries answered turn bursting off,
//      once;
//   H) begin() closes the burst window and reports an open late answer;
//   I) a longer answer deadline lengthens the watchdog by as much;
//   J) ENQ_TO_SEND and ACK_TO_TELEGRAM hold the write, and the Result states
//      them;
//   K) a byte buffered when the deadline is found passed is stale: it goes
//      to the late-answer report, and the next read waits for a fresh ENQ;
//   L) the settings: the two timing points clamped to 100 ms, the other
//      points refused, the answer deadline clamped to 50..2000 ms, all 0 and
//      250 ms by default; the variant switches.
//
// Built by build_and_run_protocols.sh.

#include <cstdio>
#include <vector>

#include "fake_optolink.h"
#include "optolink_gwg.h"

namespace optolink = esphome::vitohome::optolink;
using Engine = optolink::GwgExchange<FakeOptolink>;
using optolink::GWGAccessMode;
using optolink::Lane;
using optolink::OptolinkResult;
using optolink::TimingPoint;
using Bytes = std::vector<uint8_t>;

namespace {

int g_fail = 0;

void check(bool ok, const char *what) {
  std::printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_fail;
}

struct Seen {
  int count = 0;
  optolink::Result last{};
  Bytes data;
  int fallbacks = 0;
  uint8_t fallback_failures = 0;
  int lates = 0;
  optolink::GwgLateAnswer late{};
};

void watch(Engine &e, Seen &seen) {
  e.on_result([&seen](const optolink::Result &r) {
    ++seen.count;
    seen.last = r;
    seen.data.assign(r.data, r.data + (r.data != nullptr ? r.length : 0));
  });
  e.on_burst_fallback([&seen](uint8_t failures) {
    ++seen.fallbacks;
    seen.fallback_failures = failures;
  });
  e.on_late_answer([&seen](const optolink::GwgLateAnswer &late) {
    ++seen.lates;
    seen.late = late;
  });
}

void pump(Engine &e, int n = 8) {
  for (int i = 0; i < n; ++i)
    e.loop();
}

void advance(Engine &e, uint32_t ms) {
  optolink::optolink_test_clock_advance(ms);
  pump(e);
}

bool read(Engine &e, uint16_t address, uint8_t length, GWGAccessMode mode = GWGAccessMode::PHYSICAL) {
  return e.submit(optolink::make_request(Lane::GWG, optolink::gwgReadTypeByte(mode), address, length, nullptr));
}

// The ENQ, read in one pass: what that pass wrote.
Bytes enq(Engine &e, FakeOptolink &io) {
  io.clear_written();
  io.feed({0x05});
  e.loop();
  Bytes out = io.written();
  io.clear_written();
  return out;
}

// One pass: what it wrote.
Bytes pass(Engine &e, FakeOptolink &io) {
  io.clear_written();
  e.loop();
  Bytes out = io.written();
  io.clear_written();
  return out;
}

// A read of 0x10 answered 12 34 on an ENQ: the burst window open.
void answered_read(Engine &e, FakeOptolink &io) {
  read(e, 0x10, 2);
  enq(e, io);
  io.feed({0x12, 0x34});
  pump(e);
}

}  // namespace

int main() {
  optolink::optolink_test_clock_freeze();

  // --- A ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    check(read(e, 0x10, 2), "A: a physical read of 0x10, 2 bytes, taken");
    pump(e);
    check(io.written().empty(), "A: nothing before the ENQ");
    check(enq(e, io) == Bytes({0x01, 0xCB, 0x10, 0x02, 0x04}), "A: 01 CB 10 02 04 in the ENQ's pass");
    optolink::optolink_test_clock_advance(30);
    io.feed({0x12, 0x34});
    pump(e);
    check(seen.count == 1 && seen.last.status == OptolinkResult::OK, "A: OK");
    check(seen.last.address == 0x10 && seen.data == Bytes({0x12, 0x34}), "A: 0x10 = 12 34");
    check(seen.last.timing.enq_paced && seen.last.timing.enq_to_send_ms == 0, "A: sent on the ENQ, at once");
    check(seen.last.timing.answer_timed && seen.last.timing.send_to_answer_ms == 30, "A: answered 30 ms after it");
    check(!seen.last.never_sent && !seen.last.unserved_eeprom && seen.last.retries == 0, "A: no other flag");
    check(!e.busy(), "A: idle again");
  }

  // --- B ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    answered_read(e, io);
    optolink::optolink_test_clock_advance(optolink::GWG_BURST_WINDOW_MS);
    check(read(e, 0x20, 1), "B: a read at GWG_BURST_WINDOW_MS after the answer");
    check(pass(e, io) == Bytes({0xCB, 0x20, 0x01, 0x04}), "B: CB 20 01 04 at once, no 01");
    io.feed({0x07});
    pump(e);
    check(seen.count == 2 && seen.data == Bytes({0x07}), "B: 0x20 = 07");
    check(!seen.last.timing.enq_paced, "B: burst-sent");
    optolink::optolink_test_clock_advance(optolink::GWG_BURST_WINDOW_MS + 1);
    read(e, 0x21, 1);
    check(pass(e, io).empty(), "B: one ms past the window: nothing");
    check(enq(e, io) == Bytes({0x01, 0xCB, 0x21, 0x01, 0x04}), "B: 01 CB 21 01 04 on the ENQ");
  }

  // --- C ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    read(e, 0x10, 1);
    advance(e, optolink::GWG_WATCHDOG_MS);
    check(seen.count == 0, "C: no ENQ: busy at GWG_WATCHDOG_MS");
    advance(e, 1);
    check(seen.count == 1 && seen.last.status == OptolinkResult::TIMEOUT, "C: TIMEOUT one ms later");
    check(seen.last.never_sent, "C: never_sent");
    read(e, 0x11, 1);
    enq(e, io);
    advance(e, optolink::GWG_ANSWER_TIMEOUT_MS + 1);
    check(seen.count == 2 && seen.last.status == OptolinkResult::TIMEOUT, "C: sent, unanswered: TIMEOUT");
    check(!seen.last.never_sent, "C: not never_sent");
  }

  // --- D ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    check(read(e, 0x16, 2, GWGAccessMode::EEPROM), "D: an EEPROM read of 0x16, 2 cells, taken");
    check(enq(e, io) == Bytes({0x01, 0xAE, 0x16, 0x01, 0x04}), "D: 01 AE 16 01 04 on the ENQ");
    io.feed({0xD6, 0x29});
    check(pass(e, io).empty() && seen.count == 0, "D: the first cell answered: no result");
    check(pass(e, io) == Bytes({0xAE, 0x17, 0x01, 0x04}), "D: AE 17 01 04 on the next pass, burst-sent");
    io.feed({0xFF, 0xFF});
    pump(e);
    check(seen.count == 1 && seen.last.status == OptolinkResult::OK, "D: OK");
    check(seen.data == Bytes({0x29, 0xFF}) && seen.last.length == 2, "D: 29 FF");
    check(seen.last.unserved_eeprom, "D: unserved_eeprom for FF FF");
    read(e, 0x18, 1, GWGAccessMode::EEPROM);
    pass(e, io);
    io.feed({0x12, 0x34});
    pump(e);
    check(seen.count == 2 && seen.last.status == OptolinkResult::CRC, "D: 12 34: CRC");
  }

  // --- E ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    check(!e.submit(optolink::make_request(Lane::P300, 0x01, 0x10, 1, nullptr)), "E: the P300 lane refused");
    check(!e.submit(optolink::make_request(Lane::KW, 0xF7, 0x10, 1, nullptr)), "E: the KW lane refused");
    check(!read(e, 0xFF, 2, GWGAccessMode::EEPROM), "E: an EEPROM run past 0xFF refused");
    check(!read(e, 0x10, optolink::MAX_PAYLOAD + 1), "E: a read past MAX_PAYLOAD refused");
    check(read(e, 0x10, 1) && !read(e, 0x11, 1), "E: a second read while busy refused");
    bool inner = true;
    e.on_result([&](const optolink::Result &) { inner = read(e, 0x12, 1); });
    enq(e, io);
    io.feed({0x01});
    pump(e);
    check(!inner, "E: a read from inside the result callback refused");
    check(!e.busy() && read(e, 0x12, 1), "E: taken after it");
  }

  // --- F ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    const uint8_t pair[2] = {0xD6, 0x29};
    check(e.submit(optolink::make_request(Lane::GWG, 0xAD, 0x16, 2, pair)), "F: AD with two bytes taken");
    check(enq(e, io) == Bytes({0x01, 0xAD, 0x16, 0xD6, 0x29, 0x04}), "F: 01 AD 16 D6 29 04 on the ENQ");
    io.feed({0x00});
    pump(e);
    check(seen.count == 1 && seen.last.status == OptolinkResult::OK && seen.data == Bytes({0x00}), "F: ack 00");
    check(!seen.last.timing.answer_timed, "F: a write's ack is not timed");
  }

  // --- G ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    for (int round = 1; round <= 2; ++round) {
      answered_read(e, io);
      read(e, 0x30, 1);
      pass(e, io);
      advance(e, optolink::GWG_ANSWER_TIMEOUT_MS + 1);
      if (round == 1) {
        check(seen.count == 1, "G: burst read silent: no result");
        check(enq(e, io) == Bytes({0x01, 0xCB, 0x30, 0x01, 0x04}), "G: 01 CB 30 01 04 on the next ENQ");
        check(seen.lates == 1 && seen.late.retried && seen.late.address == 0x30, "G: reported, retried");
      } else {
        enq(e, io);
      }
      io.feed({0x42});
      pump(e);
      check(seen.last.status == OptolinkResult::OK && seen.data == Bytes({0x42}),
            round == 1 ? "G: answered on the ENQ" : "G: the second retry answered too");
      seen.count = 0;
    }
    check(seen.fallbacks == 1 && seen.fallback_failures == optolink::GWG_BURST_FAILURES,
          "G: bursting off, reported once with GWG_BURST_FAILURES");
    answered_read(e, io);
    read(e, 0x31, 1);
    check(pass(e, io).empty(), "G: a read inside the window waits for the ENQ");
  }

  // --- H ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    answered_read(e, io);
    e.begin();
    read(e, 0x20, 2);
    check(pass(e, io).empty(), "H: after begin() a read in the window waits for the ENQ");
    check(enq(e, io) == Bytes({0x01, 0xCB, 0x20, 0x02, 0x04}), "H: 01 CB 20 02 04 on the ENQ");
    io.feed({0x01});
    pump(e);
    advance(e, optolink::GWG_ANSWER_TIMEOUT_MS + 1);
    check(seen.last.status == OptolinkResult::TIMEOUT && seen.lates == 0, "H: one byte of two: TIMEOUT");
    e.begin();
    check(seen.lates == 1 && seen.late.received == 1, "H: begin() reports the open late answer");
  }

  // --- I ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    uint32_t deadline = 1000;
    e.set_window(optolink::LinkWindow::ANSWER_DEADLINE, deadline);
    e.begin();
    read(e, 0x10, 1);
    advance(e, optolink::GWG_WATCHDOG_MS + 750);
    check(seen.count == 0, "I: deadline 1000 ms: busy at GWG_WATCHDOG_MS + 750");
    advance(e, 1);
    check(seen.count == 1 && seen.last.never_sent, "I: TIMEOUT one ms later");
  }

  // --- J ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    uint32_t delay = 30;
    uint32_t gap = 20;
    e.set_timing(TimingPoint::ENQ_TO_SEND, delay);
    e.set_timing(TimingPoint::ACK_TO_TELEGRAM, gap);
    e.begin();
    read(e, 0x10, 1);
    check(enq(e, io).empty() && e.wants_fast_loop(), "J: the ENQ held, the loop kept fast");
    optolink::optolink_test_clock_advance(29);
    check(pass(e, io).empty(), "J: nothing at 29 ms");
    optolink::optolink_test_clock_advance(1);
    check(pass(e, io) == Bytes({0x01}), "J: the 01 alone at 30 ms");
    optolink::optolink_test_clock_advance(19);
    check(pass(e, io).empty(), "J: nothing 19 ms after it");
    optolink::optolink_test_clock_advance(1);
    check(pass(e, io) == Bytes({0xCB, 0x10, 0x01, 0x04}) && !e.wants_fast_loop(), "J: the rest 20 ms after it");
    io.feed({0x05});
    pump(e);
    check(seen.count == 1 && seen.data == Bytes({0x05}), "J: 0x10 = 05");
    check(seen.last.timing.enq_to_send_ms == 30 && seen.last.timing.ack_gap_ms == 20, "J: the Result states 30 and 20");
  }

  // --- K ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    read(e, 0x10, 1);
    enq(e, io);
    optolink::optolink_test_clock_advance(optolink::GWG_ANSWER_TIMEOUT_MS + 1);
    io.feed({0x05});
    e.loop();
    check(seen.count == 1 && seen.last.status == OptolinkResult::TIMEOUT,
          "K: an ENQ buffered past the deadline: TIMEOUT");
    read(e, 0x11, 1);
    check(pass(e, io).empty(), "K: the next read does not go out on it");
    check(enq(e, io) == Bytes({0x01, 0xCB, 0x11, 0x01, 0x04}), "K: 01 CB 11 01 04 on a fresh ENQ");
    check(seen.lates == 1 && seen.late.late_count == 1 && seen.late.late[0] == 0x05, "K: the stale 05 reported late");
    check(seen.late.first_late_ms == optolink::GWG_ANSWER_TIMEOUT_MS + 1, "K: at GWG_ANSWER_TIMEOUT_MS + 1");
  }

  // --- L ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    check(e.timing(TimingPoint::ENQ_TO_SEND) == 0 && e.timing(TimingPoint::ACK_TO_TELEGRAM) == 0 &&
              e.timing_default(TimingPoint::ENQ_TO_SEND) == 0,
          "L: both timing points 0 by default");
    uint32_t ms = 150;
    check(e.set_timing(TimingPoint::ENQ_TO_SEND, ms) && ms == optolink::ENQ_TO_SEND_MAX_MS &&
              e.timing(TimingPoint::ENQ_TO_SEND) == optolink::ENQ_TO_SEND_MAX_MS,
          "L: ENQ_TO_SEND 150 ms clamped to 100");
    ms = 150;
    check(e.set_timing(TimingPoint::ACK_TO_TELEGRAM, ms) && ms == optolink::ACK_TO_TELEGRAM_MAX_MS,
          "L: ACK_TO_TELEGRAM 150 ms clamped to 100");
    bool refused = true;
    for (TimingPoint point : {TimingPoint::ANSWER_TO_NEXT, TimingPoint::ENQ_TO_SYNC, TimingPoint::FRAME_TO_ACK}) {
      ms = 10;
      refused = refused && !e.set_timing(point, ms) && e.timing(point) == 0;
    }
    check(refused, "L: ANSWER_TO_NEXT, ENQ_TO_SYNC, FRAME_TO_ACK refused");
    check(e.window(optolink::LinkWindow::ANSWER_DEADLINE) == optolink::GWG_ANSWER_TIMEOUT_MS &&
              e.window_default(optolink::LinkWindow::ANSWER_DEADLINE) == optolink::GWG_ANSWER_TIMEOUT_MS,
          "L: the answer deadline 250 ms by default");
    ms = 10;
    check(e.set_window(optolink::LinkWindow::ANSWER_DEADLINE, ms) && ms == optolink::GWG_ANSWER_TIMEOUT_MIN_MS,
          "L: 10 ms clamped to 50");
    ms = 5000;
    check(e.set_window(optolink::LinkWindow::ANSWER_DEADLINE, ms) && ms == optolink::GWG_ANSWER_TIMEOUT_MAX_MS &&
              e.window(optolink::LinkWindow::ANSWER_DEADLINE) == optolink::GWG_ANSWER_TIMEOUT_MAX_MS,
          "L: 5000 ms clamped to 2000");
    check(e.write_eot_before_payload() == optolink::GWG_DEFAULT_WRITE_EOT_BEFORE_PAYLOAD &&
              e.burst_omits_enq_ack() == optolink::GWG_DEFAULT_BURST_OMITS_ENQ_ACK,
          "L: the variant switches at their defaults");
    e.set_write_eot_before_payload(true);
    e.set_burst_omits_enq_ack(false);
    check(e.write_eot_before_payload() && !e.burst_omits_enq_ack(), "L: and set");
  }

  std::printf("GWG exchange: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
