// Proof for the KW/VS1 GFA read (PacketVS1Type.GFA_READ, 0x6B): a read in the
// burner control's address space. Shaped after the hardware-confirmed series of
// 2026-10-26 (VScotHO1_72, burner control 0x24), which read
//   F7 76 50 01 -> 24   (the Vitotronic's GFA_Kennung)
//   6B 40 50 01 -> 24   (P80 in the burner control, the same chip byte)
//   6B 40 06 01 -> 0F   (P06)
// and sent every 0x6B telegram 25 ms after the previous answer.
//
// Pinned below:
//   (1) a GFA read that opens a chain goes out on the ENQ like any read:
//       01 6B <addr> <len>, answered by <len> raw bytes;
//   (2) a READ chained after it goes out at once (the pause is GFA's alone);
//   (3) a GFA read chained after an answer waits exactly GFA_GAP_MS;
//   (4) a multi-byte GFA read completes on <len> bytes;
//   (5) the chain guard still holds during the pause: a byte arriving then is
//       taken as the sync it is, and the GFA read goes out on it;
//   (6) single-in-flight and the packet guards cover gfaRead() too.
//
// Built with -DVITOHOME_PROTOCOL_KW by build_and_run_protocols.sh. Time is
// injected (optolink_test_clock_freeze), so (3) pins both sides of the pause.
#include <cstdio>
#include <vector>

#include "fake_optolink.h"
#include "protocol_select.h"

using namespace esphome::vitohome;
using Engine = optolink::OptolinkEngine<SelectedProtocol>;

namespace {
int g_responses = 0;
int g_errors = 0;
std::vector<uint8_t> g_payload;
uint16_t g_address = 0;
}  // namespace

static void pump(Engine &a, int n = 8) {
  for (int i = 0; i < n; ++i)
    a.loop();
}

int main() {
  optolink::optolink_test_clock_freeze();

  FakeOptolink uart;
  Engine adapter(&uart);
  adapter.onResponse([](const uint8_t *d, uint8_t len, uint16_t address) {
    g_responses++;
    g_payload.assign(d, d + len);
    g_address = address;
  });
  adapter.onError([](optolink::OptolinkResult, uint16_t) { g_errors++; });
  adapter.begin();

  int failures = 0;
  auto check = [&failures](bool ok, const char *what) {
    std::printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
      failures++;
  };

  static_assert(optolink::PacketVS1Type.GFA_READ == 0x6B, "GFA_READ is the 0x6B type");

  // --- (1) a GFA read opens the chain ----------------------------------------
  check(adapter.gfaRead(0x4050, 1), "gfa read accepted");
  uart.feed({0x05});
  pump(adapter);
  check(uart.written() == std::vector<uint8_t>({0x01, 0x6B, 0x40, 0x50, 0x01}), "first telegram is 01 6B 40 50 01");
  uart.clear_written();
  uart.feed({0x24});
  pump(adapter);
  check(g_responses == 1 && g_payload == std::vector<uint8_t>({0x24}), "P80 answer 24 delivered");
  check(g_address == 0x4050, "  with the request address");

  // --- (2) a read chained after it: no pause ---------------------------------
  check(adapter.read(0x7650, 1), "chained read accepted");
  pump(adapter);
  check(uart.written() == std::vector<uint8_t>({0xF7, 0x76, 0x50, 0x01}), "F7 76 50 01 goes out at once");
  uart.clear_written();
  uart.feed({0x24});
  pump(adapter);
  check(g_responses == 2, "  and completes");

  // --- (3) a chained GFA read waits GFA_GAP_MS --------------------------------
  constexpr uint32_t gap = optolink::VS1Engine::GFA_GAP_MS;
  check(adapter.gfaRead(0x4006, 1), "chained gfa read accepted");
  pump(adapter);
  check(uart.written().empty(), "nothing sent right after the answer");
  optolink::optolink_test_clock_advance(gap - 1);
  pump(adapter);
  check(uart.written().empty(), "nothing sent 1 ms before the pause ends");
  optolink::optolink_test_clock_advance(1);
  pump(adapter);
  check(uart.written() == std::vector<uint8_t>({0x6B, 0x40, 0x06, 0x01}), "6B 40 06 01 once the pause is over");
  uart.clear_written();
  uart.feed({0x0F});
  pump(adapter);
  check(g_responses == 3 && g_payload == std::vector<uint8_t>({0x0F}), "P06 answer 0F delivered");

  // --- (4) a two-byte GFA read ------------------------------------------------
  check(adapter.gfaRead(0x408C, 2), "two-byte gfa read accepted");
  optolink::optolink_test_clock_advance(gap);
  pump(adapter);
  check(uart.written() == std::vector<uint8_t>({0x6B, 0x40, 0x8C, 0x02}), "6B 40 8C 02");
  uart.clear_written();
  uart.feed({0x12});
  pump(adapter);
  check(g_responses == 3, "one byte is not yet an answer");
  uart.feed({0x34});
  pump(adapter);
  check(g_responses == 4 && g_payload == std::vector<uint8_t>({0x12, 0x34}), "both bytes delivered");

  // --- (5) the chain guard during the pause -----------------------------------
  check(adapter.gfaRead(0x4009, 1), "gfa read accepted");
  pump(adapter);
  check(uart.written().empty(), "waiting out the pause");
  uart.feed({0x05});  // the device speaks before the pause ends
  pump(adapter);
  check(uart.written() == std::vector<uint8_t>({0x01, 0x6B, 0x40, 0x09, 0x01}), "the byte is a sync: 01 6B 40 09 01");
  uart.clear_written();
  uart.feed({0x10});
  pump(adapter);
  check(g_responses == 5 && g_payload == std::vector<uint8_t>({0x10}), "answer is the data byte, not the 0x05");

  // --- (6) guards --------------------------------------------------------------
  check(adapter.read(0x0800, 2), "read accepted");
  check(!adapter.gfaRead(0x4050, 1), "gfa read refused while a request is in flight");
  optolink::PacketVS1 packet;
  check(!packet.createPacket(optolink::PacketVS1Type.GFA_READ, 0x4050, 0), "zero-length gfa read refused");
  check(packet.createPacket(optolink::PacketVS1Type.GFA_READ, 0x4050, 1) && packet.length() == 4,
        "gfa read frame is 4 bytes");
  check(!packet.createPacket(0x68, 0x4050, 1), "any other type is still refused");
  check(g_errors == 0, "no errors anywhere in the run");

  std::printf("vs1 gfa read: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
