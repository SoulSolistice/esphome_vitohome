// Proves two VS2 engine guards against the vendored engine (P300 build):
//
//  A. THIRD_PARTY.md #9 -- a complete frame whose PacketType is NOT RESPONSE
//     (the device ERROR type 0x03) must fire the ERROR callback, never the
//     response callback (upstream decoded and delivered it as data). The
//     link-layer choreography must stay intact: the frame is still ACKed and
//     the very next transaction must succeed.
//
//  B. THIRD_PARTY.md #10 -- a request that times out MID-FRAME must not leave
//     the byte-at-a-time parser stuck mid-parse: after the engine's own
//     timeout/RESET and a fresh handshake, the first new transaction must
//     succeed at the first attempt (upstream needed one extra failed
//     transaction to self-heal via CS_ERROR). This test sleeps past
//     REQUEST_TIMEOUT_MS (4 s) once, deliberately.
//
// Built for P300 (no protocol flag) by build_and_run_protocols.sh.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <thread>
#include <vector>

#include "fake_optolink.h"
#include "protocol_select.h"

using namespace esphome::vitohome;
using Engine = optolink::OptolinkEngine<SelectedProtocol>;  // P300: no protocol flag

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

// Drive RESET/INIT to IDLE: engine writes EOT, expects ENQ, writes SYNC,
// expects ACK.
static void handshake(Engine &a, FakeOptolink &u) {
  pump(a);         // RESET: engine writes EOT (0x04)
  u.feed({0x05});  // device ENQ
  pump(a);         // INIT: engine writes SYNC 16 00 00
  u.feed({0x06});  // device ACK
  pump(a);         // -> IDLE
  u.clear_written();
}

int main() {
  FakeOptolink uart;
  Engine adapter(&uart);
  adapter.onResponse([](const uint8_t *data, uint8_t length, uint16_t) {
    g_responses++;
    g_last_len = length;
    if (data != nullptr && length <= sizeof(g_last_payload)) {
      std::memcpy(g_last_payload, data, length);
    }
  });
  adapter.onError([](optolink::OptolinkResult e, uint16_t) {
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

  // --- A. ERROR-type frame must not be delivered as data --------------------
  check(adapter.read(0x0800, 2), "A: read accepted");
  pump(adapter);      // SENDSTART/SENDPACKET/SEND_CRC -> SEND_ACK
  uart.feed({0x06});  // device ACKs our request
  pump(adapter);      // -> RECEIVE
  // Device ERROR frame: type 0x03, fc READ, addr 0x0800, 2 payload bytes.
  uart.feed(frame({0x07, 0x03, 0x01, 0x08, 0x00, 0x02, 0xDE, 0xAD}));
  pump(adapter);
  check(g_errors == 1 && g_last_error == optolink::OptolinkResult::ERROR, "A: ERROR frame -> onError(ERROR)");
  check(g_responses == 0, "A: ERROR frame NOT delivered as response");
  check(!adapter.isBusy(), "A: engine freed for the next request");
  uart.clear_written();

  // The choreography must survive: the very next transaction succeeds.
  check(adapter.read(0x0800, 2), "A: follow-up read accepted");
  pump(adapter);
  uart.feed({0x06});
  pump(adapter);
  uart.feed(frame({0x07, 0x01, 0x01, 0x08, 0x00, 0x02, 0x01, 0x23}));  // clean RESPONSE
  pump(adapter);
  check(g_responses == 1 && g_last_len == 2 && g_last_payload[0] == 0x01 && g_last_payload[1] == 0x23,
        "A: next transaction delivers clean payload");

  // --- B. mid-frame timeout must reset the parser ---------------------------
  g_responses = 0;
  g_errors = 0;
  check(adapter.read(0x0800, 2), "B: read accepted");
  pump(adapter);
  uart.feed({0x06});              // request ACKed
  pump(adapter);                  // -> RECEIVE
  uart.feed({0x41, 0x07, 0x01});  // frame starts... then the device dies
  pump(adapter);
  std::printf("  (sleeping past REQUEST_TIMEOUT_MS = 4 s ...)\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(4200));
  pump(adapter);  // engine timeout: onError(TIMEOUT), -> RESET
  check(g_errors == 1 && g_last_error == optolink::OptolinkResult::TIMEOUT, "B: mid-frame timeout surfaced");
  handshake(adapter, uart);  // engine re-handshakes after RESET

  check(adapter.read(0x0800, 2), "B: post-recovery read accepted");
  pump(adapter);
  uart.feed({0x06});
  pump(adapter);
  uart.feed(frame({0x07, 0x01, 0x01, 0x08, 0x00, 0x02, 0x00, 0xD2}));  // clean RESPONSE
  pump(adapter);
  // Without the parser reset, the leftover mid-PAYLOAD state consumes this
  // frame's bytes as continuation and the FIRST post-recovery transaction
  // fails (CS_ERROR / mis-parse) before self-healing.
  check(g_responses == 1 && g_errors == 1, "B: FIRST post-recovery transaction succeeds");
  check(g_last_len == 2 && g_last_payload[0] == 0x00 && g_last_payload[1] == 0xD2, "B: payload = 00 D2");

  std::printf("vs2 guard proofs: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
