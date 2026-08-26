// Proves the two GWG fixes motivated by the 2026-08-23 hardware capture, both
// of which the pre-fix engine fails:
//
//   (a) RESPONSE_TIMEOUT_MS -- an unanswered read must fail as TIMEOUT, not be
//       completed by the device's next idle ENQ. GWG read responses are raw
//       data bytes with no framing, so 0x05 is indistinguishable from payload
//       by value; only timing separates them. Pre-fix, RECEIVE stayed open for
//       the full 3000 ms REQUEST_TIMEOUT_MS and handed 0x05 to the caller as a
//       successful 1-byte response (2.5 degC on any div2 temperature).
//
//   (b) same-loop send -- the request frame must be on the wire in the SAME
//       loop() iteration that consumed the device's ENQ. Pre-fix, _init()
//       returned after setting SEND and the write happened one iteration later,
//       spending part of the KW-family sync window on unrelated component work.
//
// Case 3 is the guard against over-correcting (a): 0x05 is a LEGAL datapoint
// value, so a prompt 0x05 must still be delivered as data. Any "drop leading
// ENQ" implementation passes cases 1 and 2 and fails this one.
//
// Built with -DVITOHOME_PROTOCOL_GWG by build_and_run_protocols.sh.
#include <chrono>  // NOLINT [build/c++11]
#include <cstdio>
#include <thread>  // NOLINT [build/c++11]
#include <vector>

#include "fake_optolink.h"
#include "protocol_select.h"

using namespace esphome::vitohome;
using Engine = optolink::OptolinkEngine<SelectedProtocol>;

namespace {
int g_responses = 0;
int g_errors = 0;
optolink::OptolinkResult g_last_error = optolink::OptolinkResult::CONTINUE;
uint16_t g_last_error_addr = 0;
uint8_t g_last_len = 0;
uint8_t g_last_payload[8] = {0};
}  // namespace

static void pump(Engine &a, int n = 6) {
  for (int i = 0; i < n; ++i)
    a.loop();
}

int main() {
  FakeOptolink uart;
  Engine adapter(&uart);
  adapter.onResponse([](const uint8_t *data, uint8_t length, uint16_t) {
    g_responses++;
    g_last_len = length;
    if (data != nullptr && length <= sizeof(g_last_payload))
      g_last_payload[0] = data[0];
  });
  adapter.onError([](optolink::OptolinkResult e, uint16_t addr) {
    g_errors++;
    g_last_error = e;
    g_last_error_addr = addr;
  });
  adapter.begin();

  int failures = 0;
  auto check = [&failures](bool ok, const char *what) {
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
      failures++;
  };

  // --- (b) the request must go out in the ENQ's own loop() iteration --------
  // One single loop() call: it must both consume the ENQ and write the frame.
  check(adapter.read(0x006F, 1), "read 0x6F accepted");
  uart.feed({0x05});  // device ENQ
  adapter.loop();     // exactly one iteration
  const std::vector<uint8_t> want = {0x01, 0xCB, 0x6F, 0x01, 0x04};
  check(uart.written() == want, "request written in the ENQ's own loop()");
  uart.clear_written();

  // --- (a) silent device: the next idle ENQ must NOT complete the read ------
  // Real cadence on the capture was ~1.23 s (min 0.75 s); 400 ms is past
  // RESPONSE_TIMEOUT_MS (250 ms) and well short of REQUEST_TIMEOUT_MS (3000 ms),
  // so a pass here cannot be the old 3 s watchdog firing late.
  std::printf("  (device silent; sleeping past RESPONSE_TIMEOUT_MS = 250 ms ...)\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  pump(adapter);
  check(g_errors == 1 && g_last_error == optolink::OptolinkResult::TIMEOUT, "silent device -> TIMEOUT");
  check(g_last_error_addr == 0x006F, "TIMEOUT reports the request address");
  check(g_responses == 0, "no response delivered");
  check(!adapter.isBusy(), "engine idle again after TIMEOUT");

  // The late ENQ now arrives. It must be consumed as a sync byte by INIT, never
  // retro-fitted onto the abandoned transaction.
  uart.feed({0x05});
  pump(adapter);
  check(g_responses == 0 && g_errors == 1, "late ENQ does not complete the dead read");

  // --- (a) regression guard: 0x05 is a LEGAL value when it arrives promptly --
  check(adapter.read(0x0070, 1), "read 0x70 accepted");
  uart.feed({0x05});  // ENQ -> sync + send
  adapter.loop();
  uart.feed({0x05});  // genuine payload: raw 0x05 == 2.5 degC under div2
  pump(adapter);
  check(g_responses == 1 && g_errors == 1, "prompt 0x05 completes the read");
  check(g_last_len == 1 && g_last_payload[0] == 0x05, "0x05 delivered as data, not swallowed");

  // --- (a) short multi-byte answer must fail fast, not hang to 3 s ----------
  check(adapter.read(0x0017, 2), "read 0x17 len 2 accepted");
  uart.feed({0x05});
  adapter.loop();
  uart.feed({0x63});  // only one of the two expected bytes
  pump(adapter);
  check(g_responses == 1 && g_errors == 1, "partial frame is not completed early");
  std::printf("  (short answer; sleeping past RESPONSE_TIMEOUT_MS ...)\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  pump(adapter);
  check(g_errors == 2 && g_last_error == optolink::OptolinkResult::TIMEOUT, "short answer -> TIMEOUT");
  check(g_responses == 1, "no partial payload delivered");

  // --- (a) the hostile ordering: ENQ ALREADY BUFFERED when the loop resumes --
  // The cases above all feed the late ENQ *after* the deadline has been
  // observed, which is the benign ordering. Real hardware does the opposite:
  // the ESP32 UART RX FIFO holds the idle ENQ while the host is busy (60 s
  // entity publishes, API state flush -- the very stall the capture blamed),
  // so by the time _receive() runs again the byte is already sitting there.
  //
  // If the deadline is checked AFTER the drain, this is still a misread: the
  // drain consumes 0x05, re-arms _lastMillis (so the deadline cannot fire in
  // that iteration), _bytesTransferred hits 1 == expected, and the read
  // completes with a sync byte as its payload. Checking the deadline BEFORE
  // the drain is what makes this case behave.
  const int responses_before = g_responses;
  const int errors_before = g_errors;
  check(adapter.read(0x0071, 1), "read 0x71 accepted");
  uart.feed({0x05});  // ENQ -> sync + same-loop send
  adapter.loop();
  uart.clear_written();
  std::printf("  (host stalled; idle ENQ arrives INTO THE BUFFER during the stall ...)\n");
  uart.feed({0x05});  // device's next idle ENQ, buffered while we are not looking
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  pump(adapter);  // first loop() after the stall: byte is already buffered
  check(g_responses == responses_before, "buffered idle ENQ is NOT delivered as data");
  check(g_errors == errors_before + 1 && g_last_error == optolink::OptolinkResult::TIMEOUT,
        "stalled-host misread -> TIMEOUT");
  check(g_last_error_addr == 0x0071, "TIMEOUT reports the request address");
  check(!adapter.isBusy(), "engine idle again after the stalled-host TIMEOUT");

  // The stale byte must not survive into the next transaction either: left in
  // the buffer it would be picked up by _init() as a fresh sync byte and burn
  // the next request on a window that has already closed.
  check(adapter.read(0x0072, 1), "read 0x72 accepted");
  pump(adapter);
  check(uart.written().empty(), "no request sent off the flushed stale ENQ");
  uart.feed({0x05});  // a genuinely fresh ENQ
  adapter.loop();
  const std::vector<uint8_t> want72 = {0x01, 0xCB, 0x72, 0x01, 0x04};
  check(uart.written() == want72, "next request waits for a fresh ENQ");
  uart.feed({0x21});
  pump(adapter);
  check(g_responses == responses_before + 1, "engine recovers and completes the next read");

  std::printf("gwg ENQ-misread guard: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
