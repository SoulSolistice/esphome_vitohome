// Proves the diagnostic GWG ENQ-to-send delay (optolink::gwgEnqSendDelayMs,
// issue #35, THIRD_PARTY.md #33).
//
//   * Default 0: the request goes out in the loop() pass that read the ENQ,
//     exactly as before, and enq->send reads 0.
//   * Above 0: nothing goes out until that many ms after the ENQ was read,
//     the engine reports isHoldingEnq() meanwhile, and enq->send states the
//     wait. A byte arriving during the hold is not taken for the answer.
//   * A burst-sent request does not wait and reports lastSendWasEnqPaced()
//     false.
//
// Built with -DVITOHOME_PROTOCOL_GWG by build_and_run_protocols.sh.
#include <cstdio>

#include "fake_optolink.h"
#include "protocol_select.h"

using namespace esphome::vitohome;
using Engine = optolink::OptolinkEngine<SelectedProtocol>;

namespace {
int g_responses = 0;
int g_errors = 0;
uint8_t g_last_value = 0;
}  // namespace

static void pump(Engine &a, int n = 6) {
  for (int i = 0; i < n; ++i)
    a.loop();
}

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

  // (1) Default: unchanged behaviour.
  check(optolink::gwgEnqSendDelayMs == 0, "(1) default delay is 0");
  check(adapter.read(0x55, 1), "(1) read accepted");
  uart.feed({0x05});
  adapter.loop();
  check(uart.written().size() == 5, "(1) request sent in the pass that read the ENQ");
  check(!adapter.isHoldingEnq(), "(1) never holding");
  uart.feed({0x2A});
  pump(adapter);
  check(g_responses == 1 && g_last_value == 0x2A, "(1) answered");
  check(adapter.lastSendWasEnqPaced() && adapter.lastEnqToSendMs() == 0, "(1) ENQ-paced, enq->send 0 ms");
  uart.clear_written();

  // Let the burst window lapse so the next read waits for an ENQ.
  optolink::optolink_test_clock_advance(optolink::GWGEngine::ENQ_VALIDITY_MS + 100);

  // (2) 30 ms: held, then sent; a stray byte during the hold is dropped.
  optolink::gwgEnqSendDelayMs = 30;
  check(adapter.read(0x16, 1), "(2) read accepted");
  uart.feed({0x05});
  adapter.loop();
  check(uart.written().empty(), "(2) nothing sent on the ENQ itself");
  check(adapter.isHoldingEnq(), "(2) holding the ENQ");
  optolink::optolink_test_clock_advance(29);
  uart.feed({0x05});  // stray byte during the hold: must not become the answer
  pump(adapter);
  check(uart.written().empty(), "(2) still nothing after 29 ms");
  optolink::optolink_test_clock_advance(1);
  adapter.loop();
  check(uart.written().size() == 5 && uart.written()[0] == 0x01, "(2) request sent at 30 ms, with its 0x01");
  check(!adapter.isHoldingEnq(), "(2) hold released");
  check(adapter.lastSendWasEnqPaced() && adapter.lastEnqToSendMs() == 30, "(2) enq->send 30 ms");
  uart.feed({0x2B});
  pump(adapter);
  check(g_responses == 2 && g_last_value == 0x2B, "(2) the answer, not the stray byte, is delivered");
  uart.clear_written();

  // (3) Burst: a read inside the window does not wait.
  check(adapter.read(0x17, 1), "(3) read accepted");
  adapter.loop();
  check(!uart.written().empty(), "(3) burst-sent without waiting");
  check(!adapter.lastSendWasEnqPaced(), "(3) reported as not ENQ-paced");
  uart.feed({0x2C});
  pump(adapter);
  check(g_responses == 3, "(3) answered");
  uart.clear_written();

  // (4) Unanswered after the hold: an ordinary TIMEOUT, enq->send still set.
  optolink::optolink_test_clock_advance(optolink::GWGEngine::ENQ_VALIDITY_MS + 100);
  check(adapter.read(0x18, 1), "(4) read accepted");
  uart.feed({0x05});
  adapter.loop();
  optolink::optolink_test_clock_advance(30);
  adapter.loop();
  check(uart.written().size() == 5, "(4) sent after the hold");
  optolink::optolink_test_clock_advance(optolink::gwgResponseTimeoutMs + 100);
  pump(adapter);
  check(g_errors == 1, "(4) TIMEOUT reported");
  check(adapter.lastSendWasEnqPaced() && adapter.lastEnqToSendMs() == 30, "(4) enq->send readable after it");
  check(adapter.lastEnqAgeMs() == optolink::GWGEngine::ENQ_VALIDITY_MS + 100,
        "(4) enq age (loop gap that read the ENQ) readable after it");

  optolink::gwgEnqSendDelayMs = 0;
  std::printf("gwg enq send delay: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
