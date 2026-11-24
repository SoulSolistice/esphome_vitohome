// P300 open frame (optolink_p300.h, the raw lane's `p300 f`).
//
// Pinned below:
//   (1) a frame queued before the session is up waits for it, then goes out
//       exactly as given: no 41, no checksum of the engine's own;
//   (2) a fixed answer length completes on that many raw bytes (the ACK
//       counts), at address 0; bytes past it are drained, not delivered;
//   (3) afterwards the session is re-established: EOT, once the line has
//       been quiet for P300_QUIET_MS after those bytes, ENQ, SYNC;
//   (4) collect mode (length 0) ends on a quiet line and delivers the ACK
//       and the controller's frame as they came;
//   (5) an ordinary read works after a frame;
//   (6) collect mode keeps at most FRAME_MAX bytes;
//   (7) no answer at all, or a short fixed one, is a TIMEOUT;
//   (8) the request watchdog restarts when the frame is sent, so a slow
//       handshake does not cut its answer short;
//   (9) out-of-range arguments are refused and leave the engine idle.
//
// Built (P300, the default) by build_and_run_protocols.sh.
#include <cstdio>
#include <vector>

#include "fake_optolink.h"
#include "kw_calls.h"

using namespace esphome::vitohome;
using Engine = optolink::P300Exchange<FakeOptolink>;

namespace {
int g_responses = 0;
int g_errors = 0;
std::vector<uint8_t> g_data;
uint16_t g_address = 0xFFFF;
optolink::OptolinkResult g_error = optolink::OptolinkResult::CONTINUE;
const std::vector<uint8_t> kEot = {0x04};
const std::vector<uint8_t> kSync = {0x16, 0x00, 0x00};
}  // namespace

static void pump(Engine &a, int n = 8) {
  for (int i = 0; i < n; ++i)
    a.loop();
}

// From RESET (the EOT already sent) to IDLE.
static void handshake(Engine &a, FakeOptolink &uart) {
  pump(a);
  uart.feed({0x05});
  pump(a);
  uart.feed({0x06});
  pump(a);
}

int main() {
  optolink::optolink_test_clock_freeze();

  FakeOptolink uart;
  Engine engine(&uart);
  kw_calls::wire(
      engine,
      [](const uint8_t *data, uint8_t length, uint16_t address) {
        g_responses++;
        g_data.assign(data, data + length);
        g_address = address;
      },
      [](optolink::OptolinkResult result, uint16_t) {
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

  // A Virtual_READ of 0x00F8, two bytes: 41 05 00 01 00 F8 02 00.
  const std::vector<uint8_t> read = {0x41, 0x05, 0x00, 0x01, 0x00, 0xF8, 0x02, 0x00};
  const std::vector<uint8_t> answer = {0x06, 0x41, 0x07, 0x01, 0x01, 0x00, 0xF8, 0x02, 0x20, 0xCB, 0xEE};

  // --- (1)-(3) before the session, a fixed answer ---------------------------
  check(kw_calls::frame(engine, read.data(), static_cast<uint8_t>(read.size()), 1),
        "frame accepted before the session");
  check(!kw_calls::frame(engine, read.data(), static_cast<uint8_t>(read.size()), 1), "second frame refused while busy");
  pump(engine);
  check(uart.written() == kEot, "nothing but the EOT before the session is up");
  uart.clear_written();
  handshake(engine, uart);
  std::vector<uint8_t> expect = kSync;
  expect.insert(expect.end(), read.begin(), read.end());
  check(uart.written() == expect, "SYNC, then the frame byte for byte");
  uart.clear_written();
  uart.feed(answer);
  pump(engine, 1);
  check(g_responses == 1 && g_data == std::vector<uint8_t>({0x06}) && g_address == 0, "one raw byte, at address 0");
  pump(engine);
  check(uart.written().empty(), "then RESET: the rest drained, no EOT yet");
  optolink::optolink_test_clock_advance(optolink::P300_QUIET_MS);
  pump(engine);
  check(uart.written() == kEot, "the EOT once the line was quiet");
  uart.clear_written();
  handshake(engine, uart);
  check(uart.written() == kSync, "and the session is set up again");
  uart.clear_written();

  // --- (4) collect mode -----------------------------------------------------
  check(kw_calls::frame(engine, read.data(), static_cast<uint8_t>(read.size()), 0), "collect frame accepted");
  pump(engine);
  check(uart.written() == read, "sent at once from the idle session");
  uart.clear_written();
  uart.feed({0x06});
  pump(engine);
  optolink::optolink_test_clock_advance(optolink::P300_FRAME_QUIET_MS - 100);
  uart.feed(std::vector<uint8_t>(answer.begin() + 1, answer.end()));
  pump(engine);
  check(g_responses == 1, "still collecting while bytes keep coming");
  optolink::optolink_test_clock_advance(optolink::P300_FRAME_QUIET_MS + 1);
  pump(engine);
  check(g_responses == 2 && g_data == answer && g_errors == 0, "quiet line ends it: ACK and frame, raw");
  check(uart.written() == kEot, "then RESET");
  uart.clear_written();
  handshake(engine, uart);
  uart.clear_written();

  // --- (5) an ordinary read -------------------------------------------------
  check(kw_calls::read(engine, 0x00F8, 2), "read accepted after a frame");
  pump(engine);
  check(uart.written() == read, "the engine's read is the frame above");
  uart.feed(answer);
  pump(engine);
  check(g_responses == 3 && g_data == std::vector<uint8_t>({0x20, 0xCB}) && g_address == 0x00F8,
        "and is parsed as a read again");
  uart.clear_written();

  // --- (6) collect cap ------------------------------------------------------
  check(kw_calls::frame(engine, read.data(), static_cast<uint8_t>(read.size()), 0), "collect frame accepted (cap)");
  pump(engine);
  uart.clear_written();
  std::vector<uint8_t> flood(optolink::P300_OPEN_FRAME_MAX + 20);
  for (std::size_t i = 0; i < flood.size(); ++i)
    flood[i] = static_cast<uint8_t>(i);
  uart.feed(flood);
  pump(engine);
  optolink::optolink_test_clock_advance(optolink::P300_FRAME_QUIET_MS + 1);
  pump(engine);
  check(g_responses == 4 && g_data.size() == optolink::P300_OPEN_FRAME_MAX &&
            g_data.back() == optolink::P300_OPEN_FRAME_MAX - 1,
        "a flood is cut at FRAME_MAX, the first bytes kept");
  uart.clear_written();
  handshake(engine, uart);
  uart.clear_written();

  // --- (7) no answer --------------------------------------------------------
  check(kw_calls::frame(engine, read.data(), static_cast<uint8_t>(read.size()), 0), "collect frame accepted (silent)");
  pump(engine);
  optolink::optolink_test_clock_advance(optolink::P300_FRAME_QUIET_MS + 1);
  pump(engine);
  check(g_responses == 4 && g_errors == 1 && g_error == optolink::OptolinkResult::TIMEOUT, "silence is a TIMEOUT");
  uart.clear_written();
  handshake(engine, uart);
  uart.clear_written();

  check(kw_calls::frame(engine, read.data(), static_cast<uint8_t>(read.size()), 4), "fixed frame accepted (short)");
  pump(engine);
  uart.feed({0x06, 0x41});
  pump(engine);
  optolink::optolink_test_clock_advance(optolink::P300_FRAME_QUIET_MS + 1);
  pump(engine);
  check(g_responses == 4 && g_errors == 2 && g_error == optolink::OptolinkResult::TIMEOUT,
        "a short fixed answer is a TIMEOUT");
  uart.clear_written();

  // --- (8) a slow handshake -------------------------------------------------
  check(kw_calls::frame(engine, read.data(), static_cast<uint8_t>(read.size()), 0),
        "collect frame accepted (slow handshake)");
  pump(engine);
  optolink::optolink_test_clock_advance(optolink::P300_HANDSHAKE_RETRY_MS - 100);
  uart.feed({0x05});
  pump(engine);
  optolink::optolink_test_clock_advance(optolink::P300_WATCHDOG_MS - optolink::P300_HANDSHAKE_RETRY_MS);
  uart.clear_written();
  uart.feed({0x06});
  pump(engine);
  check(uart.written() == read, "sent once the session is up, late");
  optolink::optolink_test_clock_advance(200);  // past the first deadline
  uart.feed({0x06});
  pump(engine);
  check(g_errors == 2, "no watchdog TIMEOUT while the answer comes");
  optolink::optolink_test_clock_advance(optolink::P300_FRAME_QUIET_MS + 1);
  pump(engine);
  check(g_responses == 5 && g_errors == 2 && g_data == std::vector<uint8_t>({0x06}),
        "its answer is delivered past the first watchdog deadline");
  uart.clear_written();
  handshake(engine, uart);

  // --- (9) argument checks --------------------------------------------------
  std::vector<uint8_t> long_frame(optolink::P300_OPEN_FRAME_MAX + 1, 0x41);
  check(!kw_calls::frame(engine, nullptr, 3, 1), "null bytes refused");
  check(!kw_calls::frame(engine, read.data(), 0, 1), "zero bytes refused");
  check(!kw_calls::frame(engine, long_frame.data(), static_cast<uint8_t>(long_frame.size()), 1),
        "longer than FRAME_MAX refused");
  check(!kw_calls::frame(engine, read.data(), 8, optolink::P300_OPEN_FRAME_MAX + 1), "answer past FRAME_MAX refused");
  check(kw_calls::read(engine, 0x00F8, 2), "engine still idle after the refusals");

  std::printf("p300 frame: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
