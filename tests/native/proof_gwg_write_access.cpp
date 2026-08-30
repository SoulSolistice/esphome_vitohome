// GWG write access modes (2026-08-24).
//
// Background: nothing on a GWG unit is PHYSICAL-writable. Across all 22
// Vitosoft GWG device tokens the writable datapoints are EEPROM_WRITE (1082)
// and BE_WRITE (280) -- and zero PHYSICAL_WRITE. Since 0xC8 (PHYSICAL WRITE)
// was the only write TYPE byte this engine could emit before the mode table
// existed, that is the most likely reason no GWG write has ever been confirmed
// to work anywhere, including openv issue #467.
//
// This proof pins:
//   (1) each access mode selects the documented WRITE TYPE byte;
//   (2) the two KMBUS modes, which have no write byte in the wiki table at all,
//       are REFUSED rather than silently downgraded to another mode;
//   (3) the default (2-argument-plus-data) write is byte-identical to the
//       single PHYSICAL frame this emitted before, so nothing regresses;
//   (4) the write frame layout matches gwgWriteEotBeforePayload -- the one
//       part of the GWG write frame with no evidence behind it, kept behind a
//       switch until a unit settles it;
//   (5) a write completes on one ack byte and is subject to the same
//       RESPONSE_TIMEOUT_MS deadline as a read, so an idle ENQ arriving ~1.2 s
//       later cannot pose as the ack (which is what #467's capture most likely
//       recorded).
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
optolink::OptolinkResult g_last_error = optolink::OptolinkResult::CONTINUE;
}  // namespace

static void pump(Engine &a, int n = 6) {
  for (int i = 0; i < n; ++i)
    a.loop();
}

int main() {
  // Before the engine is constructed, so its first _currentMillis sample and
  // every later one come from the same frozen clock.
  optolink::optolink_test_clock_freeze();

  FakeOptolink uart;
  Engine adapter(&uart);
  adapter.onResponse([](const uint8_t *, uint8_t, uint16_t) { g_responses++; });
  adapter.onError([](optolink::OptolinkResult e, uint16_t) {
    g_errors++;
    g_last_error = e;
  });
  adapter.begin();

  int failures = 0;
  auto check = [&failures](bool ok, const char *what) {
    std::printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
      failures++;
  };

  const uint8_t payload = 0x2A;

  // --- (3) the default write is unchanged -----------------------------------
  check(adapter.write(0x0053, &payload, 1), "write(addr, data, len) [3-arg] accepted");
  uart.feed({0x05});
  adapter.loop();
  const std::vector<uint8_t> want_physical = optolink::gwgWriteEotBeforePayload
                                                 ? std::vector<uint8_t>{0x01, 0xC8, 0x53, 0x01, 0x04, 0x2A}
                                                 : std::vector<uint8_t>{0x01, 0xC8, 0x53, 0x01, 0x2A, 0x04};
  check(uart.written() == want_physical, "3-arg write == PHYSICAL WRITE frame (unchanged)");
  uart.feed({0x00});  // ack
  pump(adapter);
  check(g_responses == 1 && g_errors == 0, "write completes on a single ack byte");
  uart.clear_written();

  // --- (5) the layout flag is RUNTIME: flipping it changes the very next
  // frame. Added 2026-08-28 when gwgWriteEotBeforePayload stopped being a
  // constexpr, so that one hardware session can try both layouts against the
  // same address instead of needing two firmware builds. This proof asserts
  // BOTH byte orders explicitly, so it also pins the two candidate layouts
  // themselves rather than only the currently-selected one.
  {
    const bool saved = optolink::gwgWriteEotBeforePayload;

    optolink::gwgWriteEotBeforePayload = false;
    check(adapter.write(0x0053, &payload, 1), "layout=false: write accepted");
    uart.feed({0x05});
    adapter.loop();
    check(uart.written() == std::vector<uint8_t>({0x01, 0xC8, 0x53, 0x01, 0x2A, 0x04}),
          "layout=false -> payload BEFORE eot");
    uart.feed({0x00});
    pump(adapter);
    uart.clear_written();

    optolink::gwgWriteEotBeforePayload = true;
    check(adapter.write(0x0053, &payload, 1), "layout=true: write accepted");
    uart.feed({0x05});
    adapter.loop();
    check(uart.written() == std::vector<uint8_t>({0x01, 0xC8, 0x53, 0x01, 0x04, 0x2A}),
          "layout=true -> eot BEFORE payload (dannerph)");
    uart.feed({0x00});
    pump(adapter);
    uart.clear_written();

    // Reads must be byte-identical under both layouts -- that is what makes it
    // safe to flip mid-session while a read-back verifies the write.
    optolink::gwgWriteEotBeforePayload = true;
    check(adapter.read(0x0053, 1), "layout=true: read accepted");
    uart.feed({0x05});
    adapter.loop();
    const std::vector<uint8_t> read_true = uart.written();
    uart.feed({0x11});
    pump(adapter);
    uart.clear_written();

    optolink::gwgWriteEotBeforePayload = false;
    check(adapter.read(0x0053, 1), "layout=false: read accepted");
    uart.feed({0x05});
    adapter.loop();
    check(uart.written() == read_true, "READ frame identical under both layouts");
    uart.feed({0x11});
    pump(adapter);
    uart.clear_written();

    optolink::gwgWriteEotBeforePayload = saved;
  }

  // --- (1) each writable mode selects the documented TYPE byte ---------------
  // Table source: openv wiki, Protokoll-GWG "Telegramm Typen" (write column),
  // cross-checked against dannerph/esphome_vitoconnect, which implements the
  // same six bytes. EEPROM and BE are the two that matter: Vitosoft marks every
  // writable GWG datapoint as one or the other.
  struct Case {
    optolink::GWGAccessMode mode;
    uint8_t expected_type;
    const char *name;
  };
  const Case cases[] = {
      {optolink::GWGAccessMode::PHYSICAL, 0xC8, "PHYSICAL -> 0xC8"},
      {optolink::GWGAccessMode::VIRTUAL, 0xC4, "VIRTUAL -> 0xC4"},
      {optolink::GWGAccessMode::EEPROM, 0xAD, "EEPROM -> 0xAD"},
      {optolink::GWGAccessMode::XRAM, 0xC3, "XRAM -> 0xC3"},
      {optolink::GWGAccessMode::PORT, 0x6D, "PORT -> 0x6D"},
      {optolink::GWGAccessMode::BE, 0x9D, "BE -> 0x9D"},
  };
  for (const auto &c : cases) {
    check(adapter.write(0x0021, &payload, 1, c.mode), c.name);
    uart.feed({0x05});
    adapter.loop();
    // (4) frame layout follows the switch; reads are identical under both.
    const std::vector<uint8_t> want = optolink::gwgWriteEotBeforePayload
                                          ? std::vector<uint8_t>{0x01, c.expected_type, 0x21, 0x01, 0x04, 0x2A}
                                          : std::vector<uint8_t>{0x01, c.expected_type, 0x21, 0x01, 0x2A, 0x04};
    check(uart.written() == want, "  frame matches table + layout switch");
    uart.feed({0x00});
    pump(adapter);
    uart.clear_written();
  }

  // --- (2) KMBUS has no write byte: refuse, do not substitute ----------------
  // Both KMBUS modes are read-only in the wiki table. Refusing is a permanent
  // rejection (engine stays idle, nothing on the wire), NOT a silent fallback
  // to PHYSICAL -- which would write to a completely different register file.
  for (const auto mode : {optolink::GWGAccessMode::KMBUS_RAM, optolink::GWGAccessMode::KMBUS_EEPROM}) {
    check(!adapter.write(0x0001, &payload, 1, mode), "KMBUS write refused");
    check(!adapter.isBusy(), "  engine not busy after the refusal");
    check(uart.written().empty(), "  nothing written to the wire");
  }
  // ... while the same mode still READS fine.
  check(adapter.read(0x0001, 1, optolink::GWGAccessMode::KMBUS_EEPROM), "KMBUS read still accepted");
  uart.feed({0x05});
  adapter.loop();
  const std::vector<uint8_t> want_kmbus_read = {0x01, 0x43, 0x01, 0x01, 0x04};
  check(uart.written() == want_kmbus_read, "  KMBUS read frame unchanged");
  uart.feed({0x11});
  pump(adapter);
  uart.clear_written();

  // --- (5) an unanswered write must TIMEOUT, not be acked by an idle ENQ -----
  // This is openv #467's capture read correctly: the unit was sent a physical
  // write, never answered, and the next idle ENQ (~1.2 s later) was taken for
  // the ack. The response deadline covers the write path too.
  const int responses_before = g_responses;
  const int errors_before = g_errors;
  check(adapter.write(0x0021, &payload, 1, optolink::GWGAccessMode::BE), "write accepted (silent device)");
  uart.feed({0x05});
  adapter.loop();
  std::printf("  (device ignores the write; idle ENQ arrives during the stall ...)\n");
  uart.feed({0x05});                           // the next idle ENQ, buffered while the host is busy
  optolink::optolink_test_clock_advance(400);  // the stall itself, past RESPONSE_TIMEOUT_MS
  pump(adapter);
  check(g_responses == responses_before, "idle ENQ is NOT accepted as a write ack");
  check(g_errors == errors_before + 1 && g_last_error == optolink::OptolinkResult::TIMEOUT,
        "unanswered write -> TIMEOUT");

  std::printf("gwg write access modes: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
