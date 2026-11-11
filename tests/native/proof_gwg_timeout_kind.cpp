// Proves GWGEngine::lastTimeoutWasUnsynced() (THIRD_PARTY.md #31), the fact the
// hub's link monitor needs and a bare TIMEOUT does not carry.
//
// GWG has no error telegram, so a TIMEOUT has two unrelated causes:
//   * the device sent no ENQ, so the request never went out -- the link is
//     down;
//   * the device took the request and did not answer -- that address is not
//     served in that access mode, and the link is fine.
// Issue #35: 54 `kmbus_eeprom` datapoints of the second kind, three of them in
// a row, published the link offline about 14 times in 6.5 minutes while the
// unit kept sending ENQs and answering everything else.
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
bool g_last_unsynced = false;
optolink::OptolinkResult g_last_error = optolink::OptolinkResult::CONTINUE;
Engine *g_engine = nullptr;
}  // namespace

static void pump(Engine &a, int n = 6) {
  for (int i = 0; i < n; ++i)
    a.loop();
}

int main() {
  optolink::optolink_test_clock_freeze();

  FakeOptolink uart;
  Engine adapter(&uart);
  g_engine = &adapter;
  adapter.onResponse([](const uint8_t *, uint8_t, uint16_t) { g_responses++; });
  // The accessor is documented as valid INSIDE the error callback, which is
  // where the hub reads it.
  adapter.onError([](optolink::OptolinkResult error, uint16_t) {
    g_errors++;
    g_last_error = error;
    g_last_unsynced = g_engine->lastTimeoutWasUnsynced();
  });
  adapter.begin();

  int failures = 0;
  auto check = [&failures](bool ok, const char *what) {
    std::printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
      failures++;
  };

  // (1) No ENQ at all: the request never leaves, and the request watchdog
  // reports it. This is the dead link.
  check(adapter.read(0x55, 1), "(1) read accepted");
  pump(adapter);
  check(uart.written().empty(), "(1) nothing sent without an ENQ");
  optolink::optolink_test_clock_advance(optolink::GWGEngine::REQUEST_TIMEOUT_MS + 100);
  pump(adapter);
  check(g_errors == 1 && g_last_error == optolink::OptolinkResult::TIMEOUT, "(1) TIMEOUT reported");
  check(g_last_unsynced, "(1) ... and it is unsynced (counts against the link)");
  check(!adapter.isBusy(), "(1) engine free again");

  // (2) An ENQ, the request goes out, no answer inside the response deadline:
  // a datapoint that is not served. The link is fine.
  check(adapter.read(0xA0, 1, optolink::GWGAccessMode::KMBUS_EEPROM), "(2) read accepted");
  check(!adapter.lastTimeoutWasUnsynced(), "(2) a new request clears the flag");
  uart.feed({0x05});
  pump(adapter);
  check(uart.written().size() == 5, "(2) request sent on the ENQ");
  uart.clear_written();
  optolink::optolink_test_clock_advance(optolink::gwgResponseTimeoutMs + 100);
  pump(adapter);
  check(g_errors == 2 && g_last_error == optolink::OptolinkResult::TIMEOUT, "(2) TIMEOUT reported");
  check(!g_last_unsynced, "(2) ... and it is NOT unsynced (link untouched)");

  // (3) A read answered on an ENQ opens the burst window. A second read rides
  // it, is not answered, and is retried on a fresh ENQ. The ENQ comes and the
  // retry goes unanswered too: still a silent datapoint, not a dead link.
  check(adapter.read(0x55, 1), "(3) read accepted");
  uart.feed({0x05});
  adapter.loop();
  uart.feed({0x2A});
  pump(adapter);
  check(g_responses == 1, "(3) first read answered");
  uart.clear_written();
  check(adapter.read(0xAA, 1, optolink::GWGAccessMode::KMBUS_EEPROM), "(3) second read accepted");
  pump(adapter);
  check(!uart.written().empty(), "(3) second read burst-sent");
  optolink::optolink_test_clock_advance(optolink::gwgResponseTimeoutMs + 100);
  pump(adapter);
  check(g_errors == 2, "(3) no TIMEOUT yet: waiting to retry on an ENQ");
  uart.feed({0x05});
  pump(adapter);
  optolink::optolink_test_clock_advance(optolink::gwgResponseTimeoutMs + 100);
  pump(adapter);
  check(g_errors == 3 && g_last_error == optolink::OptolinkResult::TIMEOUT, "(3) TIMEOUT after the retry");
  check(!g_last_unsynced, "(3) ... NOT unsynced: the retry went out on an ENQ");

  // (4) The same, but the device goes away before the retry: no ENQ ever
  // comes, the retry never leaves, and that is a link problem.
  check(adapter.read(0x55, 1), "(4) read accepted");
  uart.feed({0x05});
  adapter.loop();
  uart.feed({0x2A});
  pump(adapter);
  check(g_responses == 2, "(4) first read answered");
  check(adapter.read(0xAA, 1, optolink::GWGAccessMode::KMBUS_EEPROM), "(4) second read accepted");
  pump(adapter);
  optolink::optolink_test_clock_advance(optolink::gwgResponseTimeoutMs + 100);
  pump(adapter);
  check(g_errors == 3, "(4) no TIMEOUT yet: waiting to retry on an ENQ");
  optolink::optolink_test_clock_advance(optolink::GWGEngine::REQUEST_TIMEOUT_MS + 100);
  pump(adapter);
  check(g_errors == 4 && g_last_error == optolink::OptolinkResult::TIMEOUT, "(4) TIMEOUT: the ENQ never came");
  check(g_last_unsynced, "(4) ... and it is unsynced (counts against the link)");

  std::printf("gwg timeout kind: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
