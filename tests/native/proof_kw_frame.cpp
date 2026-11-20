// KW open frame (VS1Engine::frame(), the raw lane's `kw f`).
//
// Pinned below:
//   (1) the bytes go out exactly as given on the ENQ: no ENQ_ACK of the
//       engine's own in front;
//   (2) never chained: a chain in progress is not ridden, the frame waits
//       for an ENQ;
//   (3) a fixed answer length completes on that many bytes, at address 0;
//   (4) the next request waits for a fresh ENQ and carries its 01 again;
//   (5) collect mode (length 0) ends on a quiet line and delivers every byte;
//   (6) collect mode keeps at most kResponseBufferSize bytes;
//   (7) no answer at all, or a short fixed one, is a TIMEOUT;
//   (8) the request watchdog restarts when the frame is sent, so a late ENQ
//       does not cut its answer short;
//   (9) out-of-range arguments are refused and leave the engine idle.
//
// Built with -DVITOHOME_PROTOCOL_KW by build_and_run_protocols.sh.
#include <cstdio>
#include <vector>

#include "fake_optolink.h"
#include "protocol_select.h"

using namespace esphome::vitohome;
using Engine = optolink::OptolinkEngine<SelectedProtocol>;

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

  // A completed read leaves a chain open, which the next read would ride.
  engine.read(0x00F8, 2);
  uart.feed({0x05});
  pump(engine);
  uart.feed({0x20, 0xCB});
  pump(engine);
  uart.clear_written();
  check(g_responses == 1, "setup read completed, chain open");

  // --- (1)-(3) a fixed answer length ---------------------------------------
  const std::vector<uint8_t> read = {0x01, 0xF7, 0x00, 0xF8, 0x02};
  check(engine.frame(read.data(), static_cast<uint8_t>(read.size()), 2), "frame accepted");
  check(!engine.frame(read.data(), static_cast<uint8_t>(read.size()), 2), "second frame refused while busy");
  pump(engine);
  check(uart.written().empty(), "open chain not ridden: nothing sent before ENQ");
  uart.feed({0x05});
  pump(engine);
  check(uart.written() == read, "sent on the ENQ, byte for byte, no 01 added");
  uart.clear_written();
  uart.feed({0x20});
  pump(engine);
  check(g_responses == 1, "one byte of two: not complete yet");
  uart.feed({0xCB});
  pump(engine);
  check(g_responses == 2 && g_data == std::vector<uint8_t>({0x20, 0xCB}) && g_address == 0,
        "two bytes delivered at address 0");

  // --- (4) the next read waits for an ENQ -----------------------------------
  engine.read(0x0800, 2);
  pump(engine);
  check(uart.written().empty(), "after a frame the next read waits for ENQ");
  uart.feed({0x05});
  pump(engine);
  const std::vector<uint8_t> next = {0x01, 0xF7, 0x08, 0x00, 0x02};
  check(uart.written() == next, "and goes out with its 01 on it");
  uart.clear_written();
  uart.feed({0x11, 0x22});
  pump(engine);
  check(g_responses == 3, "that read completes");

  // --- (5) collect mode ends on a quiet line --------------------------------
  const std::vector<uint8_t> session = {0x16, 0x00, 0x00};
  check(engine.frame(session.data(), static_cast<uint8_t>(session.size()), 0), "collect frame accepted");
  uart.feed({0x05});
  pump(engine);
  check(uart.written() == session, "collect frame answers the ENQ as given");
  uart.clear_written();
  uart.feed({0x06});
  pump(engine);
  optolink::optolink_test_clock_advance(600);
  uart.feed({0x41, 0x05});
  pump(engine);
  check(g_responses == 3, "still collecting while bytes keep coming");
  optolink::optolink_test_clock_advance(optolink::VS1Engine::ANSWER_TIMEOUT_MS + 1);
  pump(engine);
  check(g_responses == 4 && g_data == std::vector<uint8_t>({0x06, 0x41, 0x05}) && g_errors == 0,
        "quiet line ends it, all three bytes delivered");

  // --- (6) collect cap ------------------------------------------------------
  check(engine.frame(read.data(), static_cast<uint8_t>(read.size()), 0), "collect frame accepted (cap)");
  uart.feed({0x05});
  pump(engine);
  uart.clear_written();
  std::vector<uint8_t> flood(optolink::VS1Engine::kResponseBufferSize + 20);
  for (std::size_t i = 0; i < flood.size(); ++i)
    flood[i] = static_cast<uint8_t>(i);
  uart.feed(flood);
  pump(engine);
  optolink::optolink_test_clock_advance(optolink::VS1Engine::ANSWER_TIMEOUT_MS + 1);
  pump(engine);
  check(g_responses == 5 && g_data.size() == optolink::VS1Engine::kResponseBufferSize &&
            g_data.back() == optolink::VS1Engine::kResponseBufferSize - 1,
        "a flood is cut at kResponseBufferSize, the first bytes kept");

  // --- (7) no answer --------------------------------------------------------
  check(engine.frame(read.data(), static_cast<uint8_t>(read.size()), 0), "collect frame accepted (silent)");
  uart.feed({0x05});
  pump(engine);
  uart.clear_written();
  optolink::optolink_test_clock_advance(optolink::VS1Engine::ANSWER_TIMEOUT_MS + 1);
  pump(engine);
  check(g_responses == 5 && g_errors == 1 && g_error == optolink::OptolinkResult::TIMEOUT, "silence is a TIMEOUT");

  check(engine.frame(read.data(), static_cast<uint8_t>(read.size()), 2), "fixed frame accepted (short)");
  uart.feed({0x05});
  pump(engine);
  uart.clear_written();
  uart.feed({0x01});
  pump(engine);
  optolink::optolink_test_clock_advance(optolink::VS1Engine::ANSWER_TIMEOUT_MS + 1);
  pump(engine);
  check(g_responses == 5 && g_errors == 2 && g_error == optolink::OptolinkResult::TIMEOUT,
        "a short fixed answer is a TIMEOUT");

  // --- (8) a late ENQ -------------------------------------------------------
  check(engine.frame(read.data(), static_cast<uint8_t>(read.size()), 0), "collect frame accepted (late ENQ)");
  optolink::optolink_test_clock_advance(optolink::VS1Engine::REQUEST_TIMEOUT_MS - 200);
  pump(engine);
  uart.clear_written();  // the EOT the engine sends while it waits
  uart.feed({0x05});
  pump(engine);
  check(uart.written() == read, "sent on the late ENQ");
  optolink::optolink_test_clock_advance(300);  // past the first deadline
  uart.feed({0xAA});
  pump(engine);
  check(g_errors == 2, "no watchdog TIMEOUT while the answer comes");
  optolink::optolink_test_clock_advance(optolink::VS1Engine::ANSWER_TIMEOUT_MS + 1);
  pump(engine);
  check(g_responses == 6 && g_errors == 2 && g_data == std::vector<uint8_t>({0xAA}),
        "its answer is delivered past the first watchdog deadline");

  // --- (9) argument checks --------------------------------------------------
  std::vector<uint8_t> long_frame(optolink::KW_FRAME_MAX + 1, 0x01);
  check(!engine.frame(nullptr, 3, 1), "null bytes refused");
  check(!engine.frame(read.data(), 0, 1), "zero bytes refused");
  check(!engine.frame(long_frame.data(), static_cast<uint8_t>(long_frame.size()), 1), "longer than the buffer refused");
  check(!engine.frame(read.data(), 5, optolink::VS1Engine::kResponseBufferSize + 1),
        "answer past kResponseBufferSize refused");
  check(engine.read(0x00F8, 2), "engine still idle after the refusals");

  std::printf("kw frame: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
