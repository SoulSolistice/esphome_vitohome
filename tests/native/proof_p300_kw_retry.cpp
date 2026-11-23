// The P300 exchange's KW silence retry (optolink_p300.h, KW_RECEIVE), on a
// frozen clock. A VScotHO1_72 ignores about 9 in 1000 first telegrams of a
// KW detour (stress run, 2026-11-23); the next ENQ follows about 2 s after
// the telegram. Pinned below:
//   A) silence until KW_ANSWER_TIMEOUT_MS: no result, the telegram again
//      with 01 on the device's next ENQ; its answer is OK with retries 1;
//   B) silence twice: TIMEOUT with retries 1, and back to P300 (EOT);
//   C) a lone ENQ in place of a 2-byte answer counts as silence;
//   D) a partial answer is not retried: TIMEOUT, retries 0;
//   E) no ENQ after the silence: TIMEOUT after KW_ENQ_TIMEOUT_MS;
//   F) a chained telegram met by silence goes out again with 01;
//   G) a write met by silence goes out again with its payload;
//   H) the watchdog runs from the retry: an ENQ just inside
//      KW_ENQ_TIMEOUT_MS is still answered;
//   I) ENQ_TO_SEND holds the resend as it holds the first send;
//   J) with SilenceRetries 0, the first silence is TIMEOUT (VS2Engine, which
//      the differential target compares against).
//
// Built by build_and_run_protocols.sh.

#include <cstdio>
#include <vector>

#include "fake_optolink.h"
#include "kw_calls.h"

namespace optolink = esphome::vitohome::optolink;
using Engine = optolink::P300Exchange<FakeOptolink>;
using EngineNoRetry = optolink::P300Exchange<FakeOptolink, 0>;
using Bytes = std::vector<uint8_t>;

namespace {

int g_fail = 0;

void check(bool ok, const char *what) {
  std::printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_fail;
}

// Every result, as delivered.
struct Seen {
  int count = 0;
  optolink::OptolinkResult status = optolink::OptolinkResult::CONTINUE;
  uint16_t address = 0;
  uint8_t retries = 0;
  Bytes data;
};

template<class E> void pump(E &e, int n = 8) {
  for (int i = 0; i < n; ++i)
    e.loop();
}

template<class E> void advance(E &e, uint32_t ms) {
  optolink::optolink_test_clock_advance(ms);
  pump(e);
}

template<class E> void watch(E &e, Seen &seen) {
  e.on_result([&seen](const optolink::Result &r) {
    ++seen.count;
    seen.status = r.status;
    seen.address = r.address;
    seen.retries = r.retries;
    seen.data.assign(r.data, r.data + (r.data != nullptr ? r.length : 0));
  });
}

// EOT, ENQ, SYNC, ACK: the link in P300.
template<class E> bool handshake(E &e, FakeOptolink &io) {
  pump(e);
  const bool eot = io.written() == Bytes({0x04});
  io.clear_written();
  io.feed({0x05});
  pump(e);
  const bool sync = io.written() == Bytes({0x16, 0x00, 0x00});
  io.clear_written();
  io.feed({0x06});
  pump(e);
  io.clear_written();
  return eot && sync;
}

// A GFA read of 0x4050 on the detour: the EOT, then the telegram on the ENQ.
template<class E> bool detour(E &e, FakeOptolink &io, uint8_t length = 1) {
  if (!kw_calls::gfa_read(e, 0x4050, length))
    return false;
  pump(e);
  const bool eot = io.written() == Bytes({0x04});
  io.clear_written();
  io.feed({0x05});
  pump(e);
  const bool sent = io.written() == Bytes({0x01, 0x6B, 0x40, 0x50, length});
  io.clear_written();
  return eot && sent;
}

// The answer deadline passes in silence.
template<class E> void silence(E &e) {
  advance(e, optolink::P300_KW_ANSWER_TIMEOUT_MS);
  advance(e, 1);
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
    check(handshake(e, io), "A: P300 link up");
    check(detour(e, io), "A: EOT, 01 6B 40 50 01 on the ENQ");
    advance(e, optolink::P300_KW_ANSWER_TIMEOUT_MS);
    check(seen.count == 0 && io.written().empty(), "A: nothing at KW_ANSWER_TIMEOUT_MS");
    advance(e, 1);
    check(seen.count == 0, "A: no result after it either");
    check(io.written().empty(), "A: no EOT, no SYNC: the detour waits");
    advance(e, 1000);
    io.feed({0x05});
    pump(e);
    check(io.written() == Bytes({0x01, 0x6B, 0x40, 0x50, 0x01}), "A: 01 6B 40 50 01 again on the next ENQ");
    io.clear_written();
    io.feed({0x24});
    pump(e);
    check(seen.count == 1 && seen.status == optolink::OptolinkResult::OK, "A: answered: OK");
    check(seen.address == 0x4050 && seen.data == Bytes({0x24}), "A: 0x4050 = 24");
    check(seen.retries == 1, "A: retries 1");
    check(!e.busy(), "A: idle again");
    check(kw_calls::gfa_read(e, 0x4006, 1), "A: the next read queued");
    advance(e, optolink::KW_GFA_GAP_MS);
    io.clear_written();
    io.feed({0x1E});
    pump(e);
    check(seen.count == 2 && seen.status == optolink::OptolinkResult::OK, "A: the next read answered");
    check(seen.retries == 0, "A: its retries 0");
  }

  // --- B ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    handshake(e, io);
    check(detour(e, io), "B: sent on the ENQ");
    silence(e);
    advance(e, 1000);
    io.feed({0x05});
    pump(e);
    check(io.written() == Bytes({0x01, 0x6B, 0x40, 0x50, 0x01}), "B: again on the next ENQ");
    io.clear_written();
    silence(e);
    check(seen.count == 1 && seen.status == optolink::OptolinkResult::TIMEOUT, "B: silence again: TIMEOUT");
    check(seen.retries == 1, "B: retries 1");
    check(io.written() == Bytes({0x04}), "B: back to P300: EOT");
  }

  // --- C ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    handshake(e, io);
    check(detour(e, io, 2), "C: 2-byte read sent on the ENQ");
    io.feed({0x05});
    pump(e);
    silence(e);
    check(seen.count == 0 && io.written().empty(), "C: a lone ENQ at the deadline: no result, no EOT");
    advance(e, 1000);
    io.feed({0x05});
    pump(e);
    check(io.written() == Bytes({0x01, 0x6B, 0x40, 0x50, 0x02}), "C: again on the next ENQ");
    io.clear_written();
    io.feed({0x12, 0x34});
    pump(e);
    check(seen.count == 1 && seen.status == optolink::OptolinkResult::OK && seen.data == Bytes({0x12, 0x34}),
          "C: answered 12 34");
    check(seen.retries == 1, "C: retries 1");
  }

  // --- D ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    handshake(e, io);
    check(detour(e, io, 2), "D: 2-byte read sent on the ENQ");
    io.feed({0x12});
    pump(e);
    silence(e);
    check(seen.count == 1 && seen.status == optolink::OptolinkResult::TIMEOUT, "D: one byte of two: TIMEOUT");
    check(seen.retries == 0, "D: retries 0");
    check(io.written() == Bytes({0x04}), "D: back to P300: EOT");
  }

  // --- E ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    handshake(e, io);
    detour(e, io);
    silence(e);
    advance(e, optolink::P300_KW_ENQ_TIMEOUT_MS);
    check(seen.count == 0, "E: no ENQ: nothing at KW_ENQ_TIMEOUT_MS");
    advance(e, 1);
    check(seen.count == 1 && seen.status == optolink::OptolinkResult::TIMEOUT, "E: TIMEOUT after it");
    check(seen.retries == 1, "E: retries 1");
  }

  // --- F ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    handshake(e, io);
    detour(e, io);
    io.feed({0x24});
    pump(e);
    check(seen.count == 1 && seen.retries == 0, "F: first read answered, retries 0");
    check(kw_calls::gfa_read(e, 0x4006, 1), "F: chained read queued");
    advance(e, optolink::KW_GFA_GAP_MS);
    check(io.written() == Bytes({0x6B, 0x40, 0x06, 0x01}), "F: 6B 40 06 01 chained");
    io.clear_written();
    silence(e);
    check(seen.count == 1 && io.written().empty(), "F: silence: no result, no EOT");
    advance(e, 1000);
    io.feed({0x05});
    pump(e);
    check(io.written() == Bytes({0x01, 0x6B, 0x40, 0x06, 0x01}), "F: again with 01 on the next ENQ");
    io.clear_written();
    io.feed({0x1E});
    pump(e);
    check(seen.count == 2 && seen.status == optolink::OptolinkResult::OK && seen.data == Bytes({0x1E}),
          "F: answered 1E");
    check(seen.retries == 1, "F: retries 1");
  }

  // --- G ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    handshake(e, io);
    const uint8_t value[1] = {0x12};
    check(kw_calls::kw_telegram(e, optolink::PacketVS1Type.WRITE, 0x2307, 1, value), "G: write queued");
    pump(e);
    io.clear_written();
    io.feed({0x05});
    pump(e);
    const Bytes telegram({0x01, 0xF4, 0x23, 0x07, 0x01, 0x12});
    check(io.written() == telegram, "G: 01 F4 23 07 01 12 on the ENQ");
    io.clear_written();
    silence(e);
    advance(e, 1000);
    io.feed({0x05});
    pump(e);
    check(io.written() == telegram, "G: the same on the next ENQ");
    io.clear_written();
    io.feed({0x00});
    pump(e);
    check(seen.count == 1 && seen.status == optolink::OptolinkResult::OK, "G: acknowledged: OK");
    check(seen.retries == 1, "G: retries 1");
  }

  // --- H ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    handshake(e, io);
    detour(e, io);
    silence(e);
    // 1 s answer deadline plus this: past the 4 s watchdog from the send.
    advance(e, optolink::P300_KW_ENQ_TIMEOUT_MS);
    check(seen.count == 0, "H: no watchdog while the retry waits for its ENQ");
    io.feed({0x05});
    pump(e);
    check(io.written() == Bytes({0x01, 0x6B, 0x40, 0x50, 0x01}), "H: sent on that late ENQ");
    io.clear_written();
    io.feed({0x24});
    pump(e);
    check(seen.count == 1 && seen.status == optolink::OptolinkResult::OK, "H: answered: OK");
  }

  // --- I ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    uint32_t hold = 30;
    e.set_timing(optolink::TimingPoint::ENQ_TO_SEND, hold);
    e.begin();
    handshake(e, io);
    check(kw_calls::gfa_read(e, 0x4050, 1), "I: read queued");
    pump(e);
    io.clear_written();
    io.feed({0x05});
    pump(e);
    advance(e, 30);
    check(io.written() == Bytes({0x01, 0x6B, 0x40, 0x50, 0x01}), "I: sent 30 ms after the ENQ");
    io.clear_written();
    silence(e);
    advance(e, 1000);
    io.feed({0x05});
    pump(e);
    advance(e, 29);
    check(io.written().empty(), "I: the resend not 29 ms after the next ENQ");
    advance(e, 1);
    check(io.written() == Bytes({0x01, 0x6B, 0x40, 0x50, 0x01}), "I: but 30 ms after it");
  }

  // --- J ------------------------------------------------------------------
  {
    FakeOptolink io;
    EngineNoRetry e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    check(handshake(e, io), "J: P300 link up");
    check(detour(e, io), "J: sent on the ENQ");
    silence(e);
    check(seen.count == 1 && seen.status == optolink::OptolinkResult::TIMEOUT, "J: no retry: TIMEOUT");
    check(seen.retries == 0, "J: retries 0");
    check(io.written() == Bytes({0x04}), "J: back to P300: EOT");
  }

  std::printf("P300 KW silence retry: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
