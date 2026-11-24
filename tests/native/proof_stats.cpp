// Proves the link counts (vito_stats.h):
//   (1) a fresh count: every key, ms "-";
//   (2) results by status, the ms over the OK results only, timed frames,
//       facts under link.<name> with spaces as _, an unknown fact ignored;
//   (3) reset() starts over and counts the seconds from it;
//   (4) a line cut short by its buffer stays terminated.
#include <cstdio>
#include <cstring>
#include <string>

#include "vito_stats.h"

using esphome::vitohome::VitoStats;
namespace optolink = esphome::vitohome::optolink;
using optolink::LinkFact;
using optolink::OptolinkResult;

namespace {

int g_failures = 0;
void check(bool ok, const char *what) {
  std::printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_failures;
}

std::string line_of(const VitoStats &s, uint32_t now_ms) {
  char line[400];
  s.format(line, sizeof(line), now_ms);
  return line;
}

}  // namespace

int main() {
  std::printf("== link counts ==\n");

  // (1)
  {
    VitoStats s;
    s.reset(1000);
    check(line_of(s, 1000) ==
              "s=0 n=0 ok=0 timeout=0 length=0 nack=0 crc=0 error=0 device_error=0 ms=- script_ok=0 script_failed=0 "
              "link.drained=0 link.retry=0 link.late=0 link.burst_off=0 link.nack=0 link.eot_refused=0 "
              "link.keepalive=0 link.eot=0 link.watchdog=0",
          "(1) every key, ms -");
  }

  // (2)
  {
    VitoStats s;
    s.reset(0);
    s.result(OptolinkResult::OK, 40);
    s.result(OptolinkResult::OK, 10);
    s.result(OptolinkResult::OK, 25);
    s.result(OptolinkResult::TIMEOUT, 3000);
    s.result(OptolinkResult::DEVICE_ERROR, 60);
    s.script(true);
    s.script(false);
    s.fact(static_cast<uint8_t>(LinkFact::RETRY));
    s.fact(static_cast<uint8_t>(LinkFact::EOT_REFUSED));
    s.fact(static_cast<uint8_t>(LinkFact::EOT_REFUSED));
    s.fact(200);
    const std::string l = line_of(s, 12500);
    check(l.rfind("s=12 n=5 ok=3 timeout=1 ", 0) == 0, "(2) 12 s, 5 requests, 3 OK, 1 timeout");
    check(l.find(" device_error=1 ") != std::string::npos, "(2) the device error");
    check(l.find(" ms=10/25/40 ") != std::string::npos, "(2) ms min/avg/max over the OK results only");
    check(l.find(" script_ok=1 script_failed=1 ") != std::string::npos, "(2) the timed frames");
    check(l.find(" link.retry=1 ") != std::string::npos && l.find(" link.eot_refused=2 ") != std::string::npos,
          "(2) facts as link.<name>, spaces as _");
    check(s.ended() == 7 && s.count(OptolinkResult::OK) == 3 && s.facts(LinkFact::EOT_REFUSED) == 2,
          "(2) the getters; an unknown fact ignored");
  }

  // (3)
  {
    VitoStats s;
    s.reset(0);
    s.result(OptolinkResult::OK, 5);
    s.reset(60000);
    check(line_of(s, 90000).rfind("s=30 n=0 ok=0 ", 0) == 0 && s.ended() == 0, "(3) reset starts over");
  }

  // (4)
  {
    VitoStats s;
    s.reset(0);
    char line[24];
    std::memset(line, 'x', sizeof(line));
    s.format(line, sizeof(line), 0);
    check(std::strlen(line) == sizeof(line) - 1 && std::string(line).rfind("s=0 n=0 ok=0", 0) == 0,
          "(4) cut short and terminated");
  }

  std::printf("link counts: %d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
