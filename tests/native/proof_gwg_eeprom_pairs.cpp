// Proof: GWG EEPROM (0xAE) reads are complement-pair aware.
//
// See gwgCellLength() (optolink_codec_gwg.h). On hardware (issue #21, GWG_VBEM 0x2053
// SW=0x7A) every EEPROM read returned two bytes, b0 + b1 == 0xFF, value in
// b1, whatever LEN the telegram carried. The engine therefore reads EEPROM
// byte-wise -- one LEN-1 telegram per address -- checks each pair, and
// delivers only the value bytes. The wire bytes used below are the ones from
// those captures.
//
// Pinned here:
//   (1) a 1-byte EEPROM read sends LEN 1, accepts the pair even when both
//       bytes arrive in one loop() (the case that used to time out), and
//       delivers b1 only;
//   (2) the same with the two bytes arriving in separate loop() calls;
//   (3) a 2-byte EEPROM datapoint is two telegrams at consecutive addresses,
//       the second burst-sent, delivered as 2 value bytes at the base address;
//   (4) a pair that does not sum to 0xFF fails the read with CRC and delivers
//       nothing;
//   (5) out-of-range EEPROM requests are refused up front, and an FF FF pair
//       ("address not in use", uwen70) is delivered as 0xFF, not as an error;
//   (6) PHYSICAL reads are untouched: LEN as requested, bytes as received;
//   (7) the timing instrument fires once per datapoint, not per pair;
//   (8) lastReadHadUnservedEeprom() (Result::unserved_eeprom) is true inside the
//       callback exactly when an FF FF pair was part of the read -- also as
//       the SECOND pair of a 2-byte read -- and false for a good EEPROM read,
//       after a reset by the next read, and for every PHYSICAL read. The
//       delivered bytes are the same either way;
//   (9) BE and PORT go one address per telegram too, one byte each (issue
//       #35: both answer a single byte whatever LEN asks for), delivered
//       together at the base address, with the same range limits.
#include <cstdio>
#include <vector>

#include "fake_optolink.h"
#include "gwg_calls.h"
#include "protocol_select.h"

using namespace esphome::vitohome;
using Engine = gwg_calls::Engine<FakeOptolink>;
using Mode = optolink::GWGAccessMode;

namespace {
int g_responses = 0;
int g_errors = 0;
int g_timing_calls = 0;
optolink::OptolinkResult g_last_error = optolink::OptolinkResult::CONTINUE;
uint16_t g_last_addr = 0;
std::vector<uint8_t> g_last_data;
// (8): sampled inside the callback, which is where the hub consumes it.
const Engine *g_engine = nullptr;
bool g_last_unserved = false;
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
    g_last_unserved = g_engine != nullptr && g_engine->lastReadHadUnservedEeprom();
  });
  adapter.onError([](optolink::OptolinkResult error, uint16_t address) {
    g_errors++;
    g_last_error = error;
    g_last_addr = address;
  });
  adapter.onTiming([](uint32_t, uint32_t, uint16_t) { g_timing_calls++; });
  adapter.begin();
  g_engine = &adapter;

  int failures = 0;
  auto check = [&failures](bool ok, const char *what) {
    std::printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
      failures++;
  };

  // --- (1) 1-byte EEPROM read, both pair bytes in one loop() ---------------
  // 2026-10-11/13: >>> 01:AE:16:02:04  <<< D7:28   (0xD7 + 0x28 == 0xFF)
  check(adapter.read(0x0016, 1, Mode::EEPROM), "(1) EEPROM read of 0x0016 accepted");
  uart.feed({0x05});
  adapter.loop();
  const std::vector<uint8_t> want16 = {0x01, 0xAE, 0x16, 0x01, 0x04};
  check(uart.written() == want16, "    telegram carries LEN 1");
  uart.clear_written();
  uart.feed({0xD7, 0x28});  // both bytes buffered before the next loop()
  pump(adapter);
  check(g_responses == 1 && g_errors == 0, "    completes on the pair, no timeout");
  check(g_last_addr == 0x0016, "    reported at the requested address");
  check(g_last_data == std::vector<uint8_t>{0x28}, "    delivers the value byte (b1) only");
  check(g_timing_calls == 1, "    (7) timing fired once");
  check(!g_last_unserved, "    (8) a good pair is not flagged unserved");

  // --- (2) same, bytes in separate loop() calls ------------------------------
  // 2026-10-11/13: <<< F0:0F at 0x000C (K45 max pump speed, 1..100 % -> 15).
  check(adapter.read(0x000C, 1, Mode::EEPROM), "(2) EEPROM read of 0x000C accepted");
  pump(adapter);  // rides the burst window opened by (1)
  const std::vector<uint8_t> want0c = {0xAE, 0x0C, 0x01, 0x04};
  check(uart.written() == want0c, "    burst-sent, LEN 1, no 0x01");
  uart.clear_written();
  uart.feed({0xF0});
  adapter.loop();
  check(g_responses == 1, "    one byte is not yet a pair");
  uart.feed({0x0F});
  pump(adapter);
  check(g_responses == 2 && g_last_data == std::vector<uint8_t>{0x0F}, "    then delivers 0x0F");

  // --- (3) 2-byte EEPROM datapoint = two telegrams ---------------------------
  // Burner hours: 0x0017 units, 0x0018 hundreds. 2026-10-13: E7 18 / F4 0B.
  const int timing_before = g_timing_calls;
  check(adapter.read(0x0017, 2, Mode::EEPROM), "(3) 2-byte EEPROM read of 0x0017 accepted");
  pump(adapter);
  const std::vector<uint8_t> want17 = {0xAE, 0x17, 0x01, 0x04};
  check(uart.written() == want17, "    first telegram: 0x0017, LEN 1");
  uart.clear_written();
  uart.feed({0xE7, 0x18});
  pump(adapter);
  check(g_responses == 2, "    not delivered after the first pair");
  const std::vector<uint8_t> want18 = {0xAE, 0x18, 0x01, 0x04};
  check(uart.written() == want18, "    second telegram: 0x0018, burst-sent, LEN 1");
  uart.clear_written();
  uart.feed({0xF4, 0x0B});
  pump(adapter);
  check(g_responses == 3 && g_errors == 0, "    delivered once both pairs are in");
  check(g_last_addr == 0x0017, "    at the base address");
  check(g_last_data == (std::vector<uint8_t>{0x18, 0x0B}), "    as the two value bytes (24 h, 11 x 100 h)");
  check(g_timing_calls == timing_before + 1, "    (7) timing fired once for the datapoint");

  // --- (4) a broken pair fails the read --------------------------------------
  check(adapter.read(0x0001, 1, Mode::EEPROM), "(4) EEPROM read accepted");
  pump(adapter);
  uart.clear_written();
  uart.feed({0x57, 0xA9});  // 0x57 + 0xA9 == 0x100, not 0xFF
  pump(adapter);
  check(g_responses == 3, "    nothing delivered");
  check(g_errors == 1 && g_last_error == optolink::OptolinkResult::CRC, "    CRC error raised");
  check(g_last_addr == 0x0001, "    for the requested address");
  check(adapter.read(0x0001, 1, Mode::EEPROM), "    engine accepts the next read");
  pump(adapter);
  uart.clear_written();
  uart.feed({0x57, 0xA8});
  pump(adapter);
  check(g_responses == 4 && g_last_data == std::vector<uint8_t>{0xA8}, "    and a good pair after it reads 0xA8");

  // A broken SECOND pair fails the whole 2-byte read.
  check(adapter.read(0x0017, 2, Mode::EEPROM), "    2-byte read accepted");
  pump(adapter);
  uart.feed({0xE7, 0x18});
  pump(adapter);
  uart.feed({0xF4, 0x0C});
  pump(adapter);
  check(g_responses == 4 && g_errors == 2 && g_last_error == optolink::OptolinkResult::CRC,
        "    a bad second pair fails the whole datapoint");
  uart.clear_written();

  // --- (5) out-of-range requests --------------------------------------------
  check(!adapter.read(0x0010, 0, Mode::EEPROM), "(5) EEPROM length 0 refused");
  check(!adapter.read(0x00FF, 2, Mode::EEPROM), "    EEPROM run past 0xFF (1-byte GWG address) refused");
  check(adapter.read(0x00FF, 1, Mode::EEPROM), "    EEPROM read of the last address accepted");
  pump(adapter);
  uart.feed({0xFF, 0xFF});  // unserved address
  pump(adapter);
  check(g_responses == 5 && g_errors == 2 && g_last_data == std::vector<uint8_t>{0xFF},
        "    FF FF pair -> 0xFF delivered, no error");
  check(g_last_unserved, "    (8) ... and flagged unserved inside the callback");
  uart.clear_written();

  // --- (6) PHYSICAL is unchanged ---------------------------------------------
  check(adapter.read(0x0017, 2, Mode::PHYSICAL), "(6) PHYSICAL 2-byte read accepted");
  pump(adapter);
  const std::vector<uint8_t> want_phys = {0xCB, 0x17, 0x02, 0x04};
  check(uart.written() == want_phys, "    LEN 2 on the wire, as requested");
  uart.clear_written();
  uart.feed({0x6C, 0x63});
  pump(adapter);
  check(g_responses == 6 && g_last_data == (std::vector<uint8_t>{0x6C, 0x63}), "    both bytes delivered as received");
  check(!g_last_unserved, "    (8) a PHYSICAL read is never flagged unserved");
  uart.clear_written();

  // --- (8) the unserved flag on a multi-byte read, and its reset --------------
  check(adapter.read(0x0017, 2, Mode::EEPROM), "(8) 2-byte EEPROM read, second address unserved");
  pump(adapter);
  uart.clear_written();
  uart.feed({0xE7, 0x18});  // good pair at 0x0017
  pump(adapter);
  uart.clear_written();
  uart.feed({0xFF, 0xFF});  // FF FF at 0x0018
  pump(adapter);
  check(g_responses == 7 && g_last_data == (std::vector<uint8_t>{0x18, 0xFF}), "    delivered as 18 FF, unchanged");
  check(g_last_unserved, "    flagged unserved (the FF FF was the second pair)");
  uart.clear_written();
  check(adapter.read(0x0016, 1, Mode::EEPROM), "    next EEPROM read accepted");
  pump(adapter);
  uart.clear_written();
  uart.feed({0xD7, 0x28});
  pump(adapter);
  check(g_responses == 8 && g_last_data == std::vector<uint8_t>{0x28}, "    delivers 0x28");
  check(!g_last_unserved, "    and the flag was reset by the new read");

  // --- (9) BE and PORT: one byte per address ---------------------------------
  const int timing_be = g_timing_calls;
  check(adapter.read(0x0010, 2, Mode::BE), "(9) 2-byte BE read of 0x0010 accepted");
  pump(adapter);
  const std::vector<uint8_t> want_be10 = {0x9E, 0x10, 0x01, 0x04};
  check(uart.written() == want_be10, "    first telegram: 0x0010, LEN 1");
  uart.clear_written();
  uart.feed({0xFF});
  pump(adapter);
  check(g_responses == 8 && g_errors == 2, "    one byte answers the first address, not delivered yet");
  const std::vector<uint8_t> want_be11 = {0x9E, 0x11, 0x01, 0x04};
  check(uart.written() == want_be11, "    second telegram: 0x0011, LEN 1");
  uart.clear_written();
  uart.feed({0xFF});
  pump(adapter);
  check(g_responses == 9 && g_errors == 2, "    delivered once both bytes are in");
  check(g_last_addr == 0x0010 && g_last_data == (std::vector<uint8_t>{0xFF, 0xFF}), "    as FF FF at the base address");
  check(!g_last_unserved, "    not flagged unserved (an EEPROM pair's flag)");
  check(g_timing_calls == timing_be + 1, "    timing fired once for the datapoint");

  check(adapter.read(0x0000, 4, Mode::PORT), "    4-byte PORT read of 0x0000 accepted");
  const uint8_t port_values[4] = {0x12, 0x34, 0x56, 0x78};
  bool port_frames = true;
  for (uint8_t i = 0; i < 4; ++i) {
    pump(adapter);
    const std::vector<uint8_t> want_port = {0x6E, i, 0x01, 0x04};
    port_frames = port_frames && uart.written() == want_port;
    uart.clear_written();
    uart.feed({port_values[i]});
  }
  pump(adapter);
  check(port_frames, "    four telegrams, 0x0000..0x0003, LEN 1 each");
  check(g_responses == 10 && g_errors == 2 && g_last_addr == 0x0000 &&
            g_last_data == (std::vector<uint8_t>{0x12, 0x34, 0x56, 0x78}),
        "    delivered as the four bytes at the base address");

  check(!adapter.read(0x0010, 0, Mode::BE), "    BE length 0 refused");
  check(!adapter.read(0x00FF, 2, Mode::PORT), "    PORT run past 0xFF refused");

  std::printf("gwg eeprom pairs: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
