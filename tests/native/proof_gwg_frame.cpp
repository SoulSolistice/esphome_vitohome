// GWG open frame (Lane::FRAME on the GWG exchange, the raw lane's `gwg f`).
//
// Pinned below:
//   (1) the bytes go out exactly as given, the leading 01 included;
//   (2) never in a burst: a valid sync is not ridden, the frame waits for ENQ;
//   (3) a fixed answer length completes on that many bytes, at address 0;
//   (4) collect mode (length 0) ends on a quiet line and delivers every byte;
//   (5) collect mode keeps at most MAX_PAYLOAD bytes;
//   (6) no answer at all is a TIMEOUT, not an empty response;
//   (7) a fixed answer length leaves no sync behind: the next read waits
//       for an ENQ, so a byte past the answer does not complete it;
//   (8) out-of-range arguments are refused and leave the engine idle.
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
std::vector<uint8_t> g_data;
uint16_t g_address = 0xFFFF;
optolink::OptolinkResult g_error = optolink::OptolinkResult::CONTINUE;
}  // namespace

static void pump(Engine &a, int n = 6) {
  for (int i = 0; i < n; ++i)
    a.loop();
}

int main() {
  optolink::optolink_test_clock_freeze();

  FakeOptolink uart;
  Engine engine(&uart);
  engine.onResponse([](const uint8_t *data, uint8_t length, uint16_t address) {
    g_responses++;
    g_data.assign(data, data + length);
    g_address = address;
  });
  engine.onError([](optolink::OptolinkResult result, uint16_t) {
    g_errors++;
    g_error = result;
  });
  engine.begin();

  int failures = 0;
  auto check = [&failures](bool ok, const char *what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
      failures++;
  };

  // A completed read leaves a valid sync behind, which a read would ride.
  engine.read(0x6F, 1);
  uart.feed({0x05});
  engine.loop();
  uart.feed({0x26});
  pump(engine);
  uart.clear_written();
  check(g_responses == 1, "setup read completed, sync valid");

  // --- (1)-(3) a fixed answer length ---------------------------------------
  const std::vector<uint8_t> km = {0x01, 0x43, 0x03, 0x00, 0x43, 0x0A, 0x01, 0x01, 0x01, 0x01, 0xE2, 0x86, 0x04};
  check(engine.frame(km.data(), static_cast<uint8_t>(km.size()), 2), "frame accepted");
  check(!engine.frame(km.data(), static_cast<uint8_t>(km.size()), 2), "second frame refused while busy");
  pump(engine);
  check(uart.written().empty(), "valid sync not ridden: nothing sent before ENQ");
  uart.feed({0x05});
  engine.loop();
  check(uart.written() == km, "sent on the ENQ, byte for byte");
  uart.clear_written();
  uart.feed({0xAA});
  pump(engine);
  check(g_responses == 1, "one byte of two: not complete yet");
  uart.feed({0xBB});
  pump(engine);
  check(g_responses == 2 && g_data == std::vector<uint8_t>({0xAA, 0xBB}) && g_address == 0,
        "two bytes delivered at address 0");

  // --- (7) no sync after a fixed answer length ------------------------------
  engine.read(0x70, 1);
  pump(engine);
  check(uart.written().empty(), "next read waits for ENQ");
  uart.feed({0xCC});  // a third answer byte
  pump(engine);
  check(uart.written().empty() && g_responses == 2 && g_errors == 0, "a surplus byte completes nothing");
  uart.feed({0x05});
  engine.loop();
  const std::vector<uint8_t> read70 = {0x01, 0xCB, 0x70, 0x01, 0x04};
  check(uart.written() == read70, "sent on the ENQ with its 01");
  uart.clear_written();
  uart.feed({0x11});
  pump(engine);
  check(g_responses == 3 && g_data == std::vector<uint8_t>({0x11}), "that read completes with its own answer");

  // --- (4) collect mode ends on a quiet line --------------------------------
  const std::vector<uint8_t> generic = {0x01, 0x43, 0x01, 0x01, 0x04};
  check(engine.frame(generic.data(), static_cast<uint8_t>(generic.size()), 0), "collect frame accepted");
  pump(engine);
  check(uart.written().empty(), "collect frame waits for ENQ too");
  uart.feed({0x05});
  engine.loop();
  check(uart.written() == generic, "collect frame sent as given");
  uart.clear_written();
  uart.feed({0x01, 0x02, 0x03});
  pump(engine);
  optolink::optolink_test_clock_advance(100);
  uart.feed({0x04, 0x05});
  pump(engine);
  check(g_responses == 3, "still collecting while bytes keep coming");
  optolink::optolink_test_clock_advance(400);  // past RESPONSE_TIMEOUT_MS (250)
  pump(engine);
  check(g_responses == 4 && g_data == std::vector<uint8_t>({0x01, 0x02, 0x03, 0x04, 0x05}) && g_errors == 0,
        "quiet line ends it, all five bytes delivered");

  // The quiet line ends the sync as well: the next read waits for an ENQ.
  engine.read(0x71, 1);
  pump(engine);
  check(uart.written().empty(), "after collect the next read waits for ENQ");
  uart.feed({0x05});
  engine.loop();
  uart.clear_written();
  uart.feed({0x22});
  pump(engine);
  check(g_responses == 5, "and completes on it");

  // --- (5) collect cap ------------------------------------------------------
  check(engine.frame(generic.data(), static_cast<uint8_t>(generic.size()), 0), "collect frame accepted (cap)");
  uart.feed({0x05});
  engine.loop();
  uart.clear_written();
  std::vector<uint8_t> flood(optolink::MAX_PAYLOAD + 20);
  for (std::size_t i = 0; i < flood.size(); ++i)
    flood[i] = static_cast<uint8_t>(i);
  uart.feed(flood);
  pump(engine);
  optolink::optolink_test_clock_advance(400);
  pump(engine);
  check(g_responses == 6 && g_data.size() == optolink::MAX_PAYLOAD && g_data.back() == optolink::MAX_PAYLOAD - 1,
        "a flood is cut at MAX_PAYLOAD, the first bytes kept");

  // --- (6) no answer --------------------------------------------------------
  check(engine.frame(generic.data(), static_cast<uint8_t>(generic.size()), 0), "collect frame accepted (silent)");
  uart.feed({0x05});
  engine.loop();
  uart.clear_written();
  optolink::optolink_test_clock_advance(400);
  pump(engine);
  check(g_responses == 6 && g_errors == 1 && g_error == optolink::OptolinkResult::TIMEOUT, "silence is a TIMEOUT");

  check(engine.frame(km.data(), static_cast<uint8_t>(km.size()), 4), "fixed frame accepted (silent)");
  uart.feed({0x05});
  engine.loop();
  uart.clear_written();
  uart.feed({0x01});
  pump(engine);
  optolink::optolink_test_clock_advance(400);
  pump(engine);
  check(g_responses == 6 && g_errors == 2 && g_error == optolink::OptolinkResult::TIMEOUT,
        "a short fixed answer is a TIMEOUT");

  // --- (8) argument checks --------------------------------------------------
  std::vector<uint8_t> long_frame(optolink::MAX_PAYLOAD + 6, 0x01);
  check(!engine.frame(nullptr, 3, 1), "null bytes refused");
  check(!engine.frame(generic.data(), 0, 1), "zero bytes refused");
  check(!engine.frame(long_frame.data(), static_cast<uint8_t>(long_frame.size()), 1), "longer than the buffer refused");
  check(!engine.frame(generic.data(), 5, optolink::MAX_PAYLOAD + 1), "answer past MAX_PAYLOAD refused");
  check(engine.read(0x72, 1), "engine still idle after the refusals");

  std::printf("gwg frame: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
