// Proves KW/VS1 WRITE completion against the vendored engine, mirroring the
// 2026-07-02 live capture from a VScotHO1_72 (0x20CB) that exposed the bug:
//
//   >>> 01:F7:08:8E:08                          (clock read, 8 bytes)
//   <<< 20:26:07:02:04:18:19:37                 (device time)
//   >>> F4:08:8E:08:20:26:07:02:04:18:19:35     (clock write, chained)
//   <<< 00                                      (single ack byte, ~125 ms)
//   [W] System-time sync: write 0x088E failed (timeout)   <-- the bug
//
// Upstream completed a WRITE on `_currentDatapoint.length()` received bytes
// (8 here), but the device acks a KW write with ONE 0x00 byte, so every
// multi-byte KW write timed out despite the device applying it. Masked for
// the common 1-byte writes where the two conventions coincide.
// THIRD_PARTY.md #11. Built with -DVITOHOME_PROTOCOL_KW.
#include <cstdio>
#include <cstring>
#include <vector>

#include "fake_optolink.h"
#include "protocol_adapter.h"

using namespace esphome::vitohome;

namespace {
int g_responses = 0;
int g_errors = 0;
uint8_t g_last_len = 0;
uint8_t g_last_payload[8] = {0};
}  // namespace

static void pump(ProtocolAdapter& a, int n = 8) {
  for (int i = 0; i < n; ++i) a.loop();
}

static bool ends_with(const std::vector<uint8_t>& hay, const std::vector<uint8_t>& tail) {
  if (hay.size() < tail.size()) return false;
  return std::equal(tail.begin(), tail.end(), hay.end() - static_cast<long>(tail.size()));
}

int main() {
  FakeOptolink uart;
  ProtocolAdapter adapter(&uart);  // VS1Engine under -DVITOHOME_PROTOCOL_KW
  adapter.on_response([](const ResponseView& r, const optolink::Datapoint&) {
    g_responses++;
    g_last_len = r.data_length;
    if (r.data != nullptr && r.data_length <= sizeof(g_last_payload)) {
      std::memcpy(g_last_payload, r.data, r.data_length);
    }
  });
  adapter.on_error([](optolink::OptolinkResult, const optolink::Datapoint&) { g_errors++; });
  adapter.begin();

  int failures = 0;
  auto check = [&failures](bool ok, const char* what) {
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
  };

  // --- the capture's clock write: 8 BCD bytes to 0x088E ---------------------
  optolink::Datapoint clock("system_time", 0x088E, 8, optolink::noconv);
  const uint8_t bcd[8] = {0x20, 0x26, 0x07, 0x02, 0x04, 0x18, 0x19, 0x35};
  check(adapter.write(clock, bcd, 8), "write accepted");
  uart.feed({0x05});  // device ENQ
  pump(adapter);
  const std::vector<uint8_t> frame = {0xF4, 0x08, 0x8E, 0x08, 0x20, 0x26, 0x07, 0x02, 0x04, 0x18, 0x19, 0x35};
  check(ends_with(uart.written(), frame), "wire frame ends with F4 08 8E 08 + BCD");
  uart.clear_written();
  uart.feed({0x00});  // the single ack byte from the capture
  pump(adapter);
  check(g_responses == 1 && g_errors == 0, "write completed on single 0x00 ack");
  check(g_last_len == 1, "ack surfaced with length 1");
  check(!adapter.is_busy(), "engine idle after write ack");

  // --- chained read afterwards (stay-synced, as in the capture) -------------
  optolink::Datapoint outside("aussentemp", 0x5525, 2, optolink::noconv);
  check(adapter.read(outside), "chained read accepted");
  pump(adapter);
  check(ends_with(uart.written(), {0xF7, 0x55, 0x25, 0x02}), "read frame = F7 55 25 02");
  uart.feed({0x54, 0x01});  // 0x0154 = 340 -> 34.0 degC, as captured
  pump(adapter);
  check(g_responses == 2 && g_errors == 0, "chained read completed");
  check(g_last_len == 2 && g_last_payload[0] == 0x54 && g_last_payload[1] == 0x01, "payload = 54 01");

  std::printf("vs1 write-ack completion: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
