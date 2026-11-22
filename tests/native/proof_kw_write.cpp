// KW write completion on the KW exchange, after a capture from a VScotHO1_72
// (0x20CB) of 2026-07-02:
//
//   >>> 01:F7:08:8E:08                          (clock read, 8 bytes)
//   <<< 20:26:07:02:04:18:19:37                 (device time)
//   >>> F4:08:8E:08:20:26:07:02:04:18:19:35     (clock write, chained)
//   <<< 00                                      (single ack byte, ~125 ms)
//
// The device acknowledges a KW write with one byte, 00 when it took it,
// whatever the write's length (THIRD_PARTY.md #11).
#include <cstdio>
#include <cstring>
#include <vector>

#include "fake_optolink.h"
#include "kw_calls.h"
#include "optolink_kw.h"

using namespace esphome::vitohome;
// Without the link's pauses, so passes count without the clock moving; the
// pauses have their own proof (proof_kw_timing.cpp).
using Engine = optolink::KwExchange<FakeOptolink, kw_calls::kNoPause>;

namespace {
int g_responses = 0;
int g_errors = 0;
uint8_t g_last_len = 0;
uint8_t g_last_payload[8] = {0};
}  // namespace

static void pump(Engine &kw, int n = 8) {
  for (int i = 0; i < n; ++i)
    kw.loop();
}

static bool ends_with(const std::vector<uint8_t> &hay, const std::vector<uint8_t> &tail) {
  if (hay.size() < tail.size())
    return false;
  return std::equal(tail.begin(), tail.end(), hay.end() - static_cast<long>(tail.size()));
}

int main() {
  FakeOptolink uart;
  Engine kw(&uart);
  kw.on_result([](const optolink::Result &r) {
    if (r.status != optolink::OptolinkResult::OK) {
      g_errors++;
      return;
    }
    g_responses++;
    g_last_len = r.length;
    if (r.data != nullptr && r.length <= sizeof(g_last_payload))
      std::memcpy(g_last_payload, r.data, r.length);
  });
  kw.begin();

  int failures = 0;
  auto check = [&failures](bool ok, const char *what) {
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
      failures++;
  };

  // --- the capture's clock write: 8 BCD bytes to 0x088E ---------------------
  const uint8_t bcd[8] = {0x20, 0x26, 0x07, 0x02, 0x04, 0x18, 0x19, 0x35};
  check(kw_calls::write(kw, 0x088E, bcd, 8), "write accepted");
  uart.feed({0x05});  // device ENQ
  pump(kw);
  const std::vector<uint8_t> frame = {0xF4, 0x08, 0x8E, 0x08, 0x20, 0x26, 0x07, 0x02, 0x04, 0x18, 0x19, 0x35};
  check(ends_with(uart.written(), frame), "wire frame ends with F4 08 8E 08 + BCD");
  uart.clear_written();
  uart.feed({0x00});  // the single ack byte from the capture
  pump(kw);
  check(g_responses == 1 && g_errors == 0, "write completed on single 0x00 ack");
  check(g_last_len == 1, "ack surfaced with length 1");
  check(!kw.busy(), "engine idle after write ack");

  // --- chained read afterwards (stay-synced, as in the capture) -------------
  check(kw_calls::read(kw, 0x5525, 2), "chained read accepted");
  pump(kw);
  check(ends_with(uart.written(), {0xF7, 0x55, 0x25, 0x02}), "read frame = F7 55 25 02");
  uart.feed({0x54, 0x01});  // 0x0154 = 340 -> 34.0 degC, as captured
  pump(kw);
  check(g_responses == 2 && g_errors == 0, "chained read completed");
  check(g_last_len == 2 && g_last_payload[0] == 0x54 && g_last_payload[1] == 0x01, "payload = 54 01");

  // --- a write answered with anything but 00 did not take -------------------
  const uint8_t one[1] = {0x03};
  check(kw_calls::write(kw, 0x2301, one, 1), "write accepted");
  pump(kw);
  uart.clear_written();
  uart.feed({0x05});  // the controller's ENQ where the ack belongs
  pump(kw);
  check(g_responses == 2 && g_errors == 1, "non-zero ack is an error, not a response");
  check(!kw.busy(), "engine idle after it");
  // The session restarts on the next ENQ.
  check(kw_calls::read(kw, 0x5525, 2), "next read accepted");
  uart.feed({0x05});
  pump(kw);
  check(ends_with(uart.written(), {0x01, 0xF7, 0x55, 0x25, 0x02}), "it goes out on a fresh ENQ (01 F7 ...)");

  std::printf("kw write-ack completion: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
