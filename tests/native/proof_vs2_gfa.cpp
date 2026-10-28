// Proof for the P300 GFA read: a KW detour (VS2Engine::gfaRead()).
//
// The controller refuses 0x6B as a P300 function code (VScotHO1_72,
// 2026-10-27: 41 05 00 6B 40 50 01 01 -> 06 41 06 03 6B 40 50 01 05 0A, an
// ERROR frame) but answers it as a KW telegram. So the engine drops P300 for
// it. Pinned below, on a frozen clock:
//   A) a GFA read leaves P300 with EOT, answers the controller's ENQ with
//      01 6B <addr> <len>, and delivers the <len> raw bytes with the address;
//   B) a GFA read right after it chains onto the KW session, GFA_GAP_MS later,
//      without a new ENQ;
//   C) with nothing queued for KW_LINGER_MS the engine re-establishes P300
//      (EOT, ENQ, SYNC 16 00 00, ACK) and reads as P300 again;
//   D) a P300 request after a GFA answer goes through the same return first;
//   E) a controller ENQ while the KW session idles is answered with SYNC;
//   F) an ENQ in the KW session opens a pending GFA read with 01 again;
//   G) no ENQ after the drop: TIMEOUT, and the engine resets to P300;
//   H) guards: length 1..KW_MAX_LENGTH, single in flight;
//   I) the request watchdog firing in the middle of a KW answer: the next
//      handshake sends exactly 16 00 00 (a stale byte count used to index
//      past SYNC; found by fuzz_raw_engines).
//
// Built by build_and_run_protocols.sh (P300 is the default protocol).

#include <cstdio>
#include <vector>

#include "fake_optolink.h"
#include "optolink/optolink.h"

namespace optolink = esphome::vitohome::optolink;
using Engine = optolink::VS2Engine;
using Bytes = std::vector<uint8_t>;

namespace {
int g_fail = 0;
int g_responses = 0;
int g_errors = 0;
optolink::OptolinkResult g_last_error = optolink::OptolinkResult::CONTINUE;
Bytes g_payload;
uint16_t g_address = 0;

void check(bool ok, const char *what) {
  std::printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_fail;
}

void pump(Engine &e, int n = 8) {
  for (int i = 0; i < n; ++i)
    e.loop();
}

void advance(Engine &e, uint32_t ms) {
  optolink::optolink_test_clock_advance(ms);
  pump(e);
}

// The P300 handshake from RESET: EOT out, ENQ in, SYNC out, ACK in.
bool handshake(Engine &e, FakeOptolink &io) {
  pump(e);
  const bool eot = io.written() == Bytes({0x04});
  io.clear_written();
  io.feed({0x05});
  pump(e);
  const bool sync = io.written() == Bytes({0x16, 0x00, 0x00});
  io.clear_written();
  io.feed({0x06});
  pump(e);
  return eot && sync;
}

// A one-byte P300 read of 0x00F8, answered.
bool p300_read(Engine &e, FakeOptolink &io) {
  const int before = g_responses;
  if (!e.read(0x00F8, 1))
    return false;
  pump(e);
  // 41 05 00 01 00 F8 01 <cs>
  const Bytes want{0x41, 0x05, 0x00, 0x01, 0x00, 0xF8, 0x01, 0xFF};
  const bool frame = io.written() == want;
  io.clear_written();
  io.feed({0x06, 0x41, 0x06, 0x01, 0x01, 0x00, 0xF8, 0x01, 0x20, 0x21});
  pump(e);
  io.clear_written();  // the engine's ACK of the answer
  return frame && g_responses == before + 1 && g_payload == Bytes({0x20});
}
}  // namespace

int main() {
  std::printf("== VS2 GFA read (KW detour) ==\n");
  optolink::optolink_test_clock_freeze();

  FakeOptolink io;
  Engine e(&io);
  e.onResponse([](const uint8_t *d, uint8_t len, uint16_t address) {
    ++g_responses;
    g_payload.assign(d, d + len);
    g_address = address;
  });
  e.onError([](optolink::OptolinkResult r, uint16_t) {
    ++g_errors;
    g_last_error = r;
  });
  e.begin();
  check(handshake(e, io), "P300 link up");

  // --- A ------------------------------------------------------------------
  check(e.gfaRead(0x4050, 1), "A: gfa read accepted");
  pump(e);
  check(io.written() == Bytes({0x04}), "A: EOT leaves P300");
  io.clear_written();
  // The controller ACKs the EOT before its ENQ (VScotHO1_72, 2026-10-27).
  io.feed({0x06});
  pump(e);
  check(io.written().empty(), "A: the ACK of the EOT is dropped");
  io.feed({0x05});
  pump(e);
  check(io.written() == Bytes({0x01, 0x6B, 0x40, 0x50, 0x01}), "A: the ENQ is answered with 01 6B 40 50 01");
  io.clear_written();
  io.feed({0x24});
  pump(e);
  check(g_responses == 1 && g_payload == Bytes({0x24}) && g_address == 0x4050, "A: P80 = 24 delivered, address 0x4050");

  // --- B ------------------------------------------------------------------
  constexpr uint32_t gap = Engine::GFA_GAP_MS;
  check(e.gfaRead(0x408C, 2), "B: chained gfa read accepted");
  pump(e);
  check(io.written().empty(), "B: nothing before the pause");
  advance(e, gap - 1);
  check(io.written().empty(), "B: nothing 1 ms before it ends");
  advance(e, 1);
  check(io.written() == Bytes({0x6B, 0x40, 0x8C, 0x02}), "B: 6B 40 8C 02 on the same session, no ENQ");
  io.clear_written();
  io.feed({0x12, 0x34});
  pump(e);
  check(g_responses == 2 && g_payload == Bytes({0x12, 0x34}), "B: both bytes delivered");

  // --- C ------------------------------------------------------------------
  advance(e, Engine::KW_LINGER_MS);
  check(io.written().empty(), "C: still in KW at KW_LINGER_MS");
  advance(e, 1);
  check(handshake(e, io), "C: after it, EOT / ENQ / SYNC / ACK back to P300");
  check(p300_read(e, io), "C: and a P300 read works");

  // --- D ------------------------------------------------------------------
  check(e.gfaRead(0x4006, 1), "D: gfa read accepted");
  pump(e);
  io.clear_written();
  io.feed({0x05});
  pump(e);
  io.clear_written();
  io.feed({0x0F});
  pump(e);
  check(g_responses == 4 && g_payload == Bytes({0x0F}), "D: P06 = 0F delivered");
  check(e.read(0x00F8, 1), "D: P300 read accepted right after");
  pump(e);
  check(handshake(e, io), "D: P300 is re-established first");
  pump(e);
  io.clear_written();
  io.feed({0x06, 0x41, 0x06, 0x01, 0x01, 0x00, 0xF8, 0x01, 0x20, 0x21});
  pump(e);
  io.clear_written();
  check(g_responses == 5 && g_payload == Bytes({0x20}), "D: then the P300 read completes");

  // --- E ------------------------------------------------------------------
  check(e.gfaRead(0x4050, 1), "E: gfa read accepted");
  pump(e);
  io.clear_written();
  io.feed({0x05});
  pump(e);
  io.clear_written();
  io.feed({0x24});
  pump(e);
  io.feed({0x05});  // the KW session idles out before KW_LINGER_MS
  pump(e);
  check(io.written() == Bytes({0x16, 0x00, 0x00}), "E: the ENQ is answered with SYNC");
  io.clear_written();
  io.feed({0x06});
  pump(e);
  check(p300_read(e, io), "E: and P300 is back");

  // --- F ------------------------------------------------------------------
  check(e.gfaRead(0x4050, 1), "F: gfa read accepted");
  pump(e);
  io.clear_written();
  io.feed({0x05});
  pump(e);
  io.clear_written();
  io.feed({0x24});
  pump(e);
  check(e.gfaRead(0x4009, 1), "F: next gfa read accepted");
  io.feed({0x05});  // an ENQ before the pause ends
  pump(e);
  check(io.written() == Bytes({0x01, 0x6B, 0x40, 0x09, 0x01}), "F: the ENQ opens it with 01 6B 40 09 01");
  io.clear_written();
  io.feed({0x10});
  pump(e);
  check(g_payload == Bytes({0x10}), "F: and its answer is the data byte");
  advance(e, Engine::KW_LINGER_MS + 1);
  check(handshake(e, io), "F: back to P300");

  // --- G ------------------------------------------------------------------
  const int errors_before = g_errors;
  check(e.gfaRead(0x4050, 1), "G: gfa read accepted");
  pump(e);
  check(io.written() == Bytes({0x04}), "G: EOT");
  io.clear_written();
  advance(e, Engine::KW_ENQ_TIMEOUT_MS);
  check(g_errors == errors_before, "G: no error at KW_ENQ_TIMEOUT_MS");
  advance(e, 1);
  check(g_errors == errors_before + 1 && g_last_error == optolink::OptolinkResult::TIMEOUT, "G: TIMEOUT after it");
  check(!e.isBusy(), "G: engine free again");
  check(handshake(e, io), "G: and it resets to P300");

  // --- H ------------------------------------------------------------------
  check(!e.gfaRead(0x4050, 0), "H: zero length refused");
  check(!e.gfaRead(0x4050, Engine::KW_MAX_LENGTH + 1), "H: more than KW_MAX_LENGTH refused");
  check(e.gfaRead(0x4050, Engine::KW_MAX_LENGTH), "H: KW_MAX_LENGTH accepted");
  check(!e.gfaRead(0x4050, 1) && !e.read(0x00F8, 1), "H: nothing else while it is in flight");

  // --- I ------------------------------------------------------------------
  {
    FakeOptolink io2;
    Engine e2(&io2);
    e2.onResponse([](const uint8_t *, uint8_t, uint16_t) { ++g_responses; });
    e2.onError([](optolink::OptolinkResult r, uint16_t) {
      ++g_errors;
      g_last_error = r;
    });
    e2.begin();
    check(handshake(e2, io2), "I: P300 link up");
    const int errors_before = g_errors;
    check(e2.gfaRead(0x4050, 4), "I: four-byte gfa read accepted");
    pump(e2);
    io2.clear_written();
    io2.feed({0x06, 0x05});
    pump(e2);
    io2.clear_written();
    io2.feed({0x11, 0x22});  // two of four bytes, then silence
    pump(e2);
    advance(e2, Engine::REQUEST_TIMEOUT_MS + 1);
    check(g_errors == errors_before + 1 && g_last_error == optolink::OptolinkResult::TIMEOUT, "I: TIMEOUT mid-answer");
    check(io2.written() == Bytes({0x04}), "I: RESET sends EOT");
    io2.clear_written();
    io2.feed({0x05});
    pump(e2);
    check(io2.written() == Bytes({0x16, 0x00, 0x00}), "I: then exactly 16 00 00");
  }

  std::printf("vs2 gfa read: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
