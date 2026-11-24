// The P300 exchange's EOT after the device's 15 (optolink_p300.h, RESET,
// RESET_ACK, KW_DROP, KW_ENQ), on a frozen clock. After its NACK a
// VScotHO1_72 refuses the next byte, an EOT too, with a 15 about 258 ms
// later (2026-11-24). Pinned below:
//   A) a restart after a NACK: the EOT refused, the device's two 15s, the
//      EOT again QuietMs after the last; the read queued meanwhile answered
//      inside its watchdog;
//   B) with QuietMs 0 (VS2Engine, which the differential target compares
//      against) the 15 is ignored, the EOT goes again after 3 s and the read
//      times out;
//   C) bytes after a checksum error: the EOT QuietMs after the last;
//   D) a 05 on the line does not hold the EOT back;
//   E) the KW detour: its EOT refused, again after QuietMs, the telegram on
//      the 05;
//   F) a device that refuses every EOT: TIMEOUT 4 s after the first drop,
//      not later;
//   G) with QuietMs 0 the detour's 15 is ignored: TIMEOUT after the ENQ wait.
//
// Built by build_and_run_protocols.sh.

#include <cstdio>
#include <vector>

#include "fake_optolink.h"
#include "kw_calls.h"

namespace optolink = esphome::vitohome::optolink;
using Engine = optolink::P300Exchange<FakeOptolink>;
using EngineOld = optolink::P300Exchange<FakeOptolink, optolink::P300_KW_SILENCE_RETRIES, 0>;
using Bytes = std::vector<uint8_t>;
using optolink::OptolinkResult;

namespace {

int g_fail = 0;

void check(bool ok, const char *what) {
  std::printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_fail;
}

struct Seen {
  int count = 0;
  OptolinkResult status = OptolinkResult::CONTINUE;
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

// A read of 0x00F8 the device NACKs.
template<class E> bool nacked_read(E &e, FakeOptolink &io, Seen &seen) {
  if (!kw_calls::read(e, 0x00F8, 4))
    return false;
  pump(e);
  const bool sent = !io.written().empty() && io.written()[0] == 0x41;
  io.clear_written();
  io.feed({0x15});
  pump(e);
  return sent && seen.count == 1 && seen.status == OptolinkResult::NACK;
}

// The ACK and answer frame of a read of 0x5525, 2 bytes: 0A 00.
const Bytes kAnswer5525 = {0x06, 0x41, 0x07, 0x01, 0x01, 0x55, 0x25, 0x02, 0x0A, 0x00, 0x8F};

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
    check(nacked_read(e, io, seen), "A: a read NACKed");
    e.begin();
    check(kw_calls::read(e, 0x5525, 2), "A: the next read queued at the restart");
    pump(e);
    check(io.written() == Bytes({0x04}), "A: the restart's EOT at once");
    io.clear_written();
    advance(e, 34);
    io.feed({0x15});
    pump(e);
    advance(e, 255);
    io.feed({0x15});
    pump(e);
    advance(e, optolink::P300_QUIET_MS - 1);
    check(io.written().empty(), "A: no EOT before QuietMs after the last 15");
    advance(e, 1);
    check(io.written() == Bytes({0x04}), "A: the EOT again at QuietMs");
    io.clear_written();
    io.feed({0x06});
    pump(e);
    advance(e, 1900);
    io.feed({0x05});
    pump(e);
    check(io.written() == Bytes({0x16, 0x00, 0x00}), "A: SYNC on the 05");
    io.clear_written();
    io.feed({0x06});
    pump(e);
    check(io.written() == Bytes({0x41, 0x05, 0x00, 0x01, 0x55, 0x25, 0x02, 0x82}), "A: the read goes out");
    io.feed(kAnswer5525);
    pump(e);
    check(seen.count == 2 && seen.status == OptolinkResult::OK && seen.data == Bytes({0x0A, 0x00}),
          "A: answered OK, 2.5 s after it was queued");
  }

  // --- B ------------------------------------------------------------------
  {
    FakeOptolink io;
    EngineOld e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    check(handshake(e, io), "B: P300 link up");
    check(nacked_read(e, io, seen), "B: a read NACKed");
    e.begin();
    check(kw_calls::read(e, 0x5525, 2), "B: the next read queued at the restart");
    pump(e);
    io.clear_written();
    advance(e, 34);
    io.feed({0x15});
    pump(e);
    advance(e, 255);
    io.feed({0x15});
    pump(e);
    advance(e, 1000);
    check(io.written().empty(), "B: QuietMs 0: the 15 ignored, no EOT");
    advance(e, optolink::P300_HANDSHAKE_RETRY_MS - 1289 + 1);
    check(io.written() == Bytes({0x04}), "B: the EOT again after 3 s");
    io.clear_written();
    io.feed({0x06});
    pump(e);
    advance(e, 1000);
    check(seen.count == 2 && seen.status == OptolinkResult::TIMEOUT, "B: the read times out before the 05");
  }

  // --- C ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    check(handshake(e, io), "C: P300 link up");
    check(kw_calls::read(e, 0x5525, 2), "C: read queued");
    pump(e);
    io.clear_written();
    Bytes bad = kAnswer5525;
    bad.back() = 0x00;
    io.feed(bad);
    pump(e);
    check(seen.count == 1 && seen.status == OptolinkResult::CRC, "C: checksum wrong: CRC");
    io.clear_written();
    advance(e, 100);
    io.feed({0x41, 0x07});
    pump(e);
    advance(e, optolink::P300_QUIET_MS - 1);
    check(io.written().empty(), "C: no EOT while the line is not quiet");
    advance(e, 1);
    check(io.written() == Bytes({0x04}), "C: the EOT QuietMs after the last byte");
  }

  // --- D ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    e.begin();
    io.feed({0x05});
    pump(e, 1);
    check(io.written() == Bytes({0x04}), "D: a 05 on the line: the EOT at once");
  }

  // --- E ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    check(handshake(e, io), "E: P300 link up");
    check(kw_calls::gfa_read(e, 0x4050, 1), "E: GFA read queued");
    pump(e);
    check(io.written() == Bytes({0x04}), "E: the detour's EOT");
    io.clear_written();
    advance(e, 258);
    io.feed({0x15});
    pump(e);
    advance(e, optolink::P300_QUIET_MS - 1);
    check(io.written().empty(), "E: refused: nothing before QuietMs");
    advance(e, 1);
    check(io.written() == Bytes({0x04}), "E: the EOT again at QuietMs");
    io.clear_written();
    io.feed({0x06});
    pump(e);
    advance(e, 1900);
    io.feed({0x05});
    pump(e);
    check(io.written() == Bytes({0x01, 0x6B, 0x40, 0x50, 0x01}), "E: the telegram on the 05");
    io.feed({0x24});
    pump(e);
    check(seen.count == 1 && seen.status == OptolinkResult::OK && seen.data == Bytes({0x24}), "E: answered OK");
  }

  // --- F ------------------------------------------------------------------
  {
    FakeOptolink io;
    Engine e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    check(handshake(e, io), "F: P300 link up");
    check(kw_calls::gfa_read(e, 0x4050, 1), "F: GFA read queued");
    pump(e);
    io.clear_written();
    uint32_t elapsed = 0;
    int eots = 1;
    while (seen.count == 0 && elapsed < 10000) {
      advance(e, 258);
      elapsed += 258;
      io.feed({0x15});
      pump(e);
      for (uint32_t i = 0; i < optolink::P300_QUIET_MS && seen.count == 0; i += 10) {
        advance(e, 10);
        elapsed += 10;
      }
      if (!io.written().empty())
        ++eots;
      io.clear_written();
    }
    check(seen.count == 1 && seen.status == OptolinkResult::TIMEOUT, "F: refused every time: TIMEOUT");
    check(elapsed > optolink::P300_WATCHDOG_MS && elapsed <= optolink::P300_WATCHDOG_MS + 600,
          "F: at the watchdog from the first drop");
    check(eots > 2, "F: the EOT sent again each time");
  }

  // --- G ------------------------------------------------------------------
  {
    FakeOptolink io;
    EngineOld e(&io);
    Seen seen;
    watch(e, seen);
    e.begin();
    check(handshake(e, io), "G: P300 link up");
    check(kw_calls::gfa_read(e, 0x4050, 1), "G: GFA read queued");
    pump(e);
    io.clear_written();
    advance(e, 258);
    io.feed({0x15});
    pump(e);
    advance(e, optolink::P300_QUIET_MS);
    check(io.written().empty(), "G: QuietMs 0: the 15 ignored");
    advance(e, optolink::P300_KW_ENQ_TIMEOUT_MS);
    check(seen.count == 1 && seen.status == OptolinkResult::TIMEOUT, "G: TIMEOUT after the ENQ wait");
  }

  std::printf("p300 eot refused: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
