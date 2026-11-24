// Proves the diagnostic GWG gap after the 0x01 (TimingPoint::ACK_TO_TELEGRAM,
// issue #35, THIRD_PARTY.md #39).
//
//   * Default 0: the request goes out as one write in the pass that read the
//     ENQ, and the measured gap reads 0.
//   * Above 0: the 0x01 goes out on the ENQ, the rest that many ms later; the
//     engine reports isHoldingEnq() meanwhile, a byte arriving in the gap is
//     not taken for the answer, and lastAckGapMs() states the gap.
//   * With an ENQ-to-send delay as well, the 0x01 waits for the delay and the
//     rest for the gap after it.
//   * A burst-sent request carries no 0x01 and does not wait.
//   * An open frame that starts with 0x01 gets the gap; one that does not goes
//     out in one write.
//
// Built with -DVITOHOME_PROTOCOL_GWG by build_and_run_protocols.sh.
#include <cstdio>
#include <vector>

#include "fake_optolink.h"
#include "gwg_calls.h"
#include "protocol_select.h"

using namespace esphome::vitohome;
using Engine = gwg_calls::Engine<FakeOptolink>;

namespace {
int g_responses = 0;
int g_errors = 0;
uint8_t g_last_value = 0;
}  // namespace

static void pump(Engine &a, int n = 6) {
  for (int i = 0; i < n; ++i)
    a.loop();
}

static void lapse_burst_window() { optolink::optolink_test_clock_advance(optolink::GWG_BURST_WINDOW_MS + 100); }

int main() {
  optolink::optolink_test_clock_freeze();

  FakeOptolink uart;
  Engine adapter(&uart);
  adapter.onResponse([](const uint8_t *data, uint8_t length, uint16_t) {
    g_responses++;
    if (length > 0)
      g_last_value = data[0];
  });
  adapter.onError([](optolink::OptolinkResult, uint16_t) { g_errors++; });
  adapter.begin();

  int failures = 0;
  auto check = [&failures](bool ok, const char *what) {
    std::printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
      failures++;
  };
  const std::vector<uint8_t> read16 = {0x01, 0xCB, 0x16, 0x01, 0x04};

  // (1) Default: one write on the ENQ.
  check(adapter.enqAckGapMs() == 0, "(1) default gap is 0");
  check(adapter.read(0x16, 1), "(1) read accepted");
  uart.feed({0x05});
  adapter.loop();
  check(uart.written() == read16, "(1) whole frame sent in the pass that read the ENQ");
  check(!adapter.isHoldingEnq(), "(1) never holding");
  uart.feed({0x2A});
  pump(adapter);
  check(g_responses == 1 && g_last_value == 0x2A, "(1) answered");
  check(adapter.lastAckGapMs() == 0, "(1) gap reads 0");
  uart.clear_written();

  // (2) 30 ms: the 0x01 on the ENQ, the rest at 30 ms; a stray byte in the gap
  // is dropped.
  lapse_burst_window();
  adapter.setEnqAckGapMs(30);
  check(adapter.read(0x16, 1), "(2) read accepted");
  uart.feed({0x05});
  adapter.loop();
  check(uart.written() == std::vector<uint8_t>{0x01}, "(2) only the 0x01 sent on the ENQ");
  check(adapter.isHoldingEnq(), "(2) holding during the gap");
  optolink::optolink_test_clock_advance(29);
  uart.feed({0x05});  // stray byte in the gap: must not become the answer
  pump(adapter);
  check(uart.written().size() == 1, "(2) nothing more after 29 ms");
  optolink::optolink_test_clock_advance(1);
  adapter.loop();
  check(uart.written() == read16, "(2) the rest sent at 30 ms");
  check(!adapter.isHoldingEnq(), "(2) gap released");
  uart.feed({0x2B});
  pump(adapter);
  check(g_responses == 2 && g_last_value == 0x2B, "(2) the answer, not the stray byte, is delivered");
  check(adapter.lastSendWasEnqPaced() && adapter.lastEnqToSendMs() == 0, "(2) enq->send 0 ms (the 0x01)");
  check(adapter.lastAckGapMs() == 30, "(2) gap 30 ms");
  uart.clear_written();

  // (3) Burst: no 0x01, no wait.
  check(adapter.read(0x17, 1), "(3) read accepted");
  adapter.loop();
  check(uart.written() == std::vector<uint8_t>({0xCB, 0x17, 0x01, 0x04}), "(3) burst-sent at once, without 0x01");
  uart.feed({0x2C});
  pump(adapter);
  check(g_responses == 3, "(3) answered");
  check(!adapter.lastSendWasEnqPaced(), "(3) its result: not ENQ-paced");
  uart.clear_written();

  // (4) Delay 20 ms and gap 30 ms: the 0x01 at 20 ms, the rest at 50 ms.
  lapse_burst_window();
  adapter.setEnqSendDelayMs(20);
  check(adapter.read(0x16, 1), "(4) read accepted");
  uart.feed({0x05});
  adapter.loop();
  check(uart.written().empty(), "(4) nothing sent on the ENQ itself");
  optolink::optolink_test_clock_advance(20);
  adapter.loop();
  check(uart.written() == std::vector<uint8_t>{0x01}, "(4) the 0x01 at 20 ms");
  optolink::optolink_test_clock_advance(29);
  adapter.loop();
  check(uart.written().size() == 1, "(4) nothing more at 49 ms");
  optolink::optolink_test_clock_advance(1);
  adapter.loop();
  check(uart.written() == read16, "(4) the rest at 50 ms");
  uart.feed({0x2D});
  pump(adapter);
  check(g_responses == 4 && g_last_value == 0x2D, "(4) answered");
  check(adapter.lastEnqToSendMs() == 20 && adapter.lastAckGapMs() == 30, "(4) enq->send 20 ms, gap 30 ms");
  adapter.setEnqSendDelayMs(0);
  uart.clear_written();

  // (5) Unanswered after the gap: an ordinary TIMEOUT, the gap still readable.
  lapse_burst_window();
  check(adapter.read(0x18, 1), "(5) read accepted");
  uart.feed({0x05});
  adapter.loop();
  optolink::optolink_test_clock_advance(30);
  adapter.loop();
  check(uart.written().size() == 5, "(5) sent after the gap");
  optolink::optolink_test_clock_advance(adapter.responseTimeoutMs() + 100);
  pump(adapter);
  check(g_errors == 1, "(5) TIMEOUT reported");
  check(adapter.lastSendWasEnqPaced() && adapter.lastAckGapMs() == 30, "(5) gap readable after it");
  uart.clear_written();

  // (6) Open frames: one starting with 0x01 gets the gap, one without does not.
  const std::vector<uint8_t> km = {0x01, 0x43, 0xF8, 0x04, 0x04};
  check(adapter.frame(km.data(), static_cast<uint8_t>(km.size()), 4), "(6) frame with 0x01 accepted");
  uart.feed({0x05});
  adapter.loop();
  check(uart.written() == std::vector<uint8_t>{0x01}, "(6) its 0x01 alone on the ENQ");
  optolink::optolink_test_clock_advance(30);
  adapter.loop();
  check(uart.written() == km, "(6) its rest at 30 ms");
  uart.feed({0x20, 0x53, 0x01, 0x1E});
  pump(adapter);
  check(g_responses == 5, "(6) answered");
  uart.clear_written();

  const std::vector<uint8_t> bare = {0x43, 0xF8, 0x04, 0x04};
  check(adapter.frame(bare.data(), static_cast<uint8_t>(bare.size()), 4), "(6) frame without 0x01 accepted");
  uart.feed({0x05});
  adapter.loop();
  check(uart.written() == bare, "(6) sent in one write");
  check(!adapter.isHoldingEnq(), "(6) no hold");
  uart.feed({0x20, 0x53, 0x01, 0x1E});
  pump(adapter);
  check(g_responses == 6, "(6) answered");
  check(adapter.lastAckGapMs() == 0, "(6) its result: no gap");

  std::printf("gwg enq ack gap: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
