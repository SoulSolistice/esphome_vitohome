// Two guards of the P300 exchange (optolink_p300.h):
//
//  A. A complete frame whose type is not RESPONSE (the error frame, type 03)
//     ends the request with DEVICE_ERROR, never as an answer. The frame is
//     still acknowledged and the next request succeeds.
//
//  B. A request that times out mid-frame leaves no parser state behind:
//     after the watchdog, RESET and a fresh handshake, the first new request
//     succeeds. The 4 s watchdog is crossed as a clock advance
//     (optolink_test_clock_freeze in optolink_clock.h).
//
// docs/p300_protocol.md, quirks 8 and 9.
#include <cstdio>
#include <cstring>
#include <numeric>
#include <vector>

#include "fake_optolink.h"
#include "kw_calls.h"

using namespace esphome::vitohome;
using Engine = optolink::P300Exchange<FakeOptolink>;

namespace {
int g_responses = 0;
int g_errors = 0;
optolink::OptolinkResult g_last_error = optolink::OptolinkResult::CONTINUE;
uint8_t g_last_payload[8] = {0};
uint8_t g_last_len = 0;
}  // namespace

static void pump(Engine &a, int n = 8) {
  for (int i = 0; i < n; ++i)
    a.loop();
}

// Wire frame after the 0x41 lead-in: {len, type, fc, addr_hi, addr_lo, plen,
// payload...} + trailing checksum (sum of everything after the lead-in).
static std::vector<uint8_t> frame(std::vector<uint8_t> body) {
  uint8_t cs = std::accumulate(body.begin(), body.end(), static_cast<uint8_t>(0));
  std::vector<uint8_t> out = {0x41};
  out.insert(out.end(), body.begin(), body.end());
  out.push_back(cs);
  return out;
}

// RESET and INIT to IDLE: the EOT, the device's ENQ, the SYNC, its ACK.
static void handshake(Engine &a, FakeOptolink &u) {
  pump(a);
  u.feed({0x05});
  pump(a);
  u.feed({0x06});
  pump(a);
  u.clear_written();
}

int main() {
  // Before the exchange is constructed: every clock sample from one frozen
  // clock.
  optolink::optolink_test_clock_freeze();

  FakeOptolink uart;
  Engine adapter(&uart);
  kw_calls::wire(
      adapter,
      [](const uint8_t *data, uint8_t length, uint16_t) {
        g_responses++;
        g_last_len = length;
        if (data != nullptr && length <= sizeof(g_last_payload)) {
          std::memcpy(g_last_payload, data, length);
        }
      },
      [](optolink::OptolinkResult e, uint16_t) {
        g_errors++;
        g_last_error = e;
      });
  adapter.begin();
  handshake(adapter, uart);

  int failures = 0;
  auto check = [&failures](bool ok, const char *what) {
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
      failures++;
  };

  // --- A. an error frame is not delivered as data ---------------------------
  check(kw_calls::read(adapter, 0x0800, 2), "A: read accepted");
  pump(adapter);      // the telegram out -> SEND_ACK
  uart.feed({0x06});  // the device's ACK
  pump(adapter);      // -> RECEIVE
  // The error frame: type 0x03, fc READ, addr 0x0800, 2 payload bytes.
  uart.feed(frame({0x07, 0x03, 0x01, 0x08, 0x00, 0x02, 0xDE, 0xAD}));
  pump(adapter);
  check(g_errors == 1 && g_last_error == optolink::OptolinkResult::DEVICE_ERROR, "A: ERROR frame -> DEVICE_ERROR");
  check(g_responses == 0, "A: ERROR frame NOT delivered as response");
  check(!adapter.busy(), "A: exchange freed for the next request");
  uart.clear_written();

  // The next request succeeds.
  check(kw_calls::read(adapter, 0x0800, 2), "A: follow-up read accepted");
  pump(adapter);
  uart.feed({0x06});
  pump(adapter);
  uart.feed(frame({0x07, 0x01, 0x01, 0x08, 0x00, 0x02, 0x01, 0x23}));  // clean RESPONSE
  pump(adapter);
  check(g_responses == 1 && g_last_len == 2 && g_last_payload[0] == 0x01 && g_last_payload[1] == 0x23,
        "A: next transaction delivers clean payload");

  // --- B. a mid-frame timeout resets the parser -----------------------------
  g_responses = 0;
  g_errors = 0;
  check(kw_calls::read(adapter, 0x0800, 2), "B: read accepted");
  pump(adapter);
  uart.feed({0x06});              // request ACKed
  pump(adapter);                  // -> RECEIVE
  uart.feed({0x41, 0x07, 0x01});  // a frame starts, then the device falls silent
  pump(adapter);
  optolink::optolink_test_clock_advance(optolink::P300_WATCHDOG_MS + 200);
  pump(adapter);  // the watchdog: TIMEOUT, -> RESET
  check(g_errors == 1 && g_last_error == optolink::OptolinkResult::TIMEOUT, "B: mid-frame timeout surfaced");
  handshake(adapter, uart);

  check(kw_calls::read(adapter, 0x0800, 2), "B: post-recovery read accepted");
  pump(adapter);
  uart.feed({0x06});
  pump(adapter);
  uart.feed(frame({0x07, 0x01, 0x01, 0x08, 0x00, 0x02, 0x00, 0xD2}));  // clean RESPONSE
  pump(adapter);
  // A parser left mid-payload would take this frame's bytes as the rest of
  // the old one and fail it.
  check(g_responses == 1 && g_errors == 1, "B: FIRST post-recovery transaction succeeds");
  check(g_last_len == 2 && g_last_payload[0] == 0x00 && g_last_payload[1] == 0xD2, "B: payload = 00 D2");

  std::printf("P300 guard proofs: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
