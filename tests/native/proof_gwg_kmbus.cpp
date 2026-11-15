// Proof: GWG KM-bus reads (KMBUS_RAM 0x33, KMBUS_EEPROM 0x43) carry a bus
// address, `01 TYPE BUS ADDR LEN`, with an optional EOT after LEN.
//
// Reported on issue #35 (uwen70): `01 33 10 00 04` reads four bytes from
// address 00 of the device at bus 10 and is answered `01 30 00 00`. That frame
// ends in its count, so whether an EOT follows is not settled;
// GWGEngine::setKmbusEot() sends either. The bus is the 16-bit address's high
// byte.
//
// Pinned here:
//   (1) the default layout is `01 TYPE BUS ADDR LEN`, answered with LEN bytes,
//       delivered at the 16-bit address;
//   (2) a burst-sent KM-bus read drops the 0x01 like every other;
//   (3) with setKmbusEot(true) the frame ends in 04, ENQ-paced and
//       burst-sent, and other reads are unchanged by the switch;
//   (4) a one-byte address goes out as bus 00;
//   (5) every other mode still refuses an address above 0xFF, and so does a
//       raw KM-bus telegram with payload;
//   (6) the raw lane's TYPE bytes take the same path;
//   (7) an unanswered KM-bus read times out like any other.
//
// Built with -DVITOHOME_PROTOCOL_GWG by build_and_run_protocols.sh.
#include <cstdio>
#include <vector>

#include "fake_optolink.h"
#include "protocol_select.h"

using namespace esphome::vitohome;
using Engine = optolink::OptolinkEngine<SelectedProtocol>;
using Mode = optolink::GWGAccessMode;

namespace {
int g_responses = 0;
int g_errors = 0;
optolink::OptolinkResult g_last_error = optolink::OptolinkResult::CONTINUE;
uint16_t g_last_addr = 0;
std::vector<uint8_t> g_last_data;
}  // namespace

static void pump(Engine &a, int n = 6) {
  for (int i = 0; i < n; ++i)
    a.loop();
}

int main() {
  optolink::optolink_test_clock_freeze();

  FakeOptolink uart;
  Engine adapter(&uart);
  adapter.onResponse([](const uint8_t *data, uint8_t length, uint16_t address) {
    g_responses++;
    g_last_addr = address;
    g_last_data.assign(data, data + length);
  });
  adapter.onError([](optolink::OptolinkResult error, uint16_t address) {
    g_errors++;
    g_last_error = error;
    g_last_addr = address;
  });
  adapter.begin();

  int failures = 0;
  auto check = [&failures](bool ok, const char *what) {
    std::printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
      failures++;
  };
  // Past ENQ_VALIDITY_MS, so the next request waits for an ENQ.
  auto close_window = [&adapter]() {
    optolink::optolink_test_clock_advance(optolink::GWGEngine::ENQ_VALIDITY_MS + 100);
    pump(adapter);
  };

  check(!adapter.kmbusEot(), "default: no EOT after the count");

  // --- (1) the reported frame ------------------------------------------------
  check(adapter.read(0x1000, 4, Mode::KMBUS_RAM), "(1) KMBUS_RAM read of bus 10, address 00 accepted");
  uart.feed({0x05});
  adapter.loop();
  check(uart.written() == std::vector<uint8_t>({0x01, 0x33, 0x10, 0x00, 0x04}), "    01 33 10 00 04");
  uart.clear_written();
  uart.feed({0x01, 0x30, 0x00, 0x00});
  pump(adapter);
  check(g_responses == 1 && g_errors == 0, "    completes on the four bytes");
  check(g_last_addr == 0x1000 && g_last_data == (std::vector<uint8_t>{0x01, 0x30, 0x00, 0x00}),
        "    delivered as 01 30 00 00 at 0x1000");

  // --- (2) burst-sent --------------------------------------------------------
  check(adapter.read(0x1001, 2, Mode::KMBUS_EEPROM), "(2) KMBUS_EEPROM read of bus 10, address 01 accepted");
  pump(adapter);
  check(uart.written() == std::vector<uint8_t>({0x43, 0x10, 0x01, 0x02}), "    burst-sent: 43 10 01 02");
  uart.clear_written();
  uart.feed({0x12, 0x34});
  pump(adapter);
  check(g_responses == 2 && g_last_addr == 0x1001 && g_last_data == (std::vector<uint8_t>{0x12, 0x34}),
        "    delivered as 12 34 at 0x1001");

  // --- (3) with the EOT ------------------------------------------------------
  adapter.setKmbusEot(true);
  check(adapter.kmbusEot(), "(3) setKmbusEot(true) takes");
  check(adapter.read(0x1000, 2, Mode::KMBUS_RAM), "    KMBUS_RAM read accepted");
  pump(adapter);
  check(uart.written() == std::vector<uint8_t>({0x33, 0x10, 0x00, 0x02, 0x04}), "    burst-sent: 33 10 00 02 04");
  uart.clear_written();
  uart.feed({0x01, 0x30});
  pump(adapter);
  check(g_responses == 3, "    completes on the two bytes");
  close_window();
  check(adapter.read(0x1001, 1, Mode::KMBUS_EEPROM), "    KMBUS_EEPROM read accepted");
  uart.feed({0x05});
  adapter.loop();
  check(uart.written() == std::vector<uint8_t>({0x01, 0x43, 0x10, 0x01, 0x01, 0x04}),
        "    on an ENQ: 01 43 10 01 01 04");
  uart.clear_written();
  uart.feed({0x55});
  pump(adapter);
  check(g_responses == 4 && g_last_data == std::vector<uint8_t>{0x55}, "    delivers 55");
  check(adapter.read(0x0010, 1, Mode::PHYSICAL), "    PHYSICAL read accepted");
  pump(adapter);
  check(uart.written() == std::vector<uint8_t>({0xCB, 0x10, 0x01, 0x04}), "    unchanged by the switch: CB 10 01 04");
  uart.clear_written();
  uart.feed({0x2A});
  pump(adapter);
  adapter.setKmbusEot(optolink::GWGEngine::DEFAULT_KMBUS_EOT);

  // --- (4) a one-byte address is bus 00 ---------------------------------------
  check(adapter.read(0x006E, 1, Mode::KMBUS_EEPROM), "(4) KMBUS_EEPROM read of 0x006E accepted");
  pump(adapter);
  check(uart.written() == std::vector<uint8_t>({0x43, 0x00, 0x6E, 0x01}), "    43 00 6E 01");
  uart.clear_written();
  uart.feed({0x5A});
  pump(adapter);
  check(g_responses == 6, "    completes");

  // --- (5) the 16-bit address is the KM-bus reads' only -----------------------
  const Mode one_byte[] = {Mode::PHYSICAL, Mode::VIRTUAL, Mode::EEPROM, Mode::XRAM, Mode::PORT, Mode::BE};
  bool refused = true;
  for (const Mode m : one_byte)
    refused = refused && !adapter.read(0x1000, 1, m);
  check(refused, "(5) every other mode refuses address 0x1000");
  const uint8_t payload = 0x01;
  check(!adapter.telegram(0x33, 0x1000, 1, &payload), "    a raw 33 telegram with payload refuses it too");
  check(!adapter.isBusy() && uart.written().empty(), "    nothing in flight, nothing written");

  // --- (6) the raw lane ------------------------------------------------------
  check(adapter.telegram(0x43, 0x1001, 1), "(6) raw telegram 43 at 0x1001 accepted");
  pump(adapter);
  check(uart.written() == std::vector<uint8_t>({0x43, 0x10, 0x01, 0x01}), "    43 10 01 01");
  uart.clear_written();
  uart.feed({0x66});
  pump(adapter);
  check(g_responses == 7 && g_last_addr == 0x1001 && g_last_data == std::vector<uint8_t>{0x66}, "    delivers 66");

  // --- (7) an unanswered KM-bus read ------------------------------------------
  close_window();
  check(adapter.read(0x2000, 1, Mode::KMBUS_RAM), "(7) KMBUS_RAM read of bus 20 accepted");
  uart.feed({0x05});
  adapter.loop();
  check(uart.written() == std::vector<uint8_t>({0x01, 0x33, 0x20, 0x00, 0x01}), "    01 33 20 00 01");
  uart.clear_written();
  optolink::optolink_test_clock_advance(optolink::GWGEngine::RESPONSE_TIMEOUT_MS + 50);
  pump(adapter);
  check(g_errors == 1 && g_last_error == optolink::OptolinkResult::TIMEOUT && g_last_addr == 0x2000,
        "    TIMEOUT at 0x2000");
  check(!adapter.isBusy(), "    and the engine is idle again");

  std::printf("gwg kmbus: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
