// Proof for the P300 remote procedure call (FunctionCode::RPC, 0x07).
//
// What the engine has to get right, and what this checks:
//   A) The request carries the parameter inline, with DataLength = the
//      parameter's length: 41 06 00 07 A0 10 01 05 <cs> for a one-byte call.
//   B) An answer whose DataLength counts the returned bytes is delivered whole.
//   C) An answer whose DataLength repeats the PARAMETER's length (1) while the
//      frame carries six bytes is delivered whole too -- the frame length and
//      the checksum decide, since the request side does not settle which of
//      the two a controller sends. The checksum still covers the byte as
//      received, so a corrupted frame of that shape is still refused.
//   D) A procedure without a parameter goes out with DataLength 0 and no
//      payload, the one request where a zero length is legal.
//   E) Reads and writes are untouched: a zero-length READ is still refused,
//      and a WRITE without data still is.
//
// Built by build_and_run.sh under ASan/UBSan with the VS2 engine sources.

#include <cstdio>
#include <vector>

#include "fake_optolink.h"
#include "optolink/optolink.h"

namespace optolink = esphome::vitohome::optolink;
using optolink::internals::ParserResult;

static int g_fail = 0;
static void check(bool ok, const char *what) {
  std::printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_fail;
}

// Start byte, then the length byte and what it counts, then their checksum.
static std::vector<uint8_t> frame(const std::vector<uint8_t> &counted) {
  std::vector<uint8_t> out{0x41};
  out.insert(out.end(), counted.begin(), counted.end());
  uint8_t cs = 0;
  for (uint8_t b : counted)
    cs = static_cast<uint8_t>(cs + b);
  out.push_back(cs);
  return out;
}

struct Result {
  std::vector<uint8_t> written;
  std::vector<uint8_t> payload;
  bool answered = false;
  bool failed = false;
  uint16_t address = 0;
};

// One RPC through a synced engine; `answer` is what the controller sends back
// after its ACK.
static Result run(uint16_t address, const std::vector<uint8_t> &parameter, const std::vector<uint8_t> &answer) {
  FakeOptolink io;
  optolink::OptolinkEngine<optolink::P300> vito(&io);
  Result r;
  vito.onResponse([&](const uint8_t *data, uint8_t length, uint16_t addr) {
    if (data)
      r.payload.assign(data, data + length);
    r.answered = true;
    r.address = addr;
  });
  vito.onError([&](optolink::OptolinkResult, uint16_t) { r.failed = true; });
  vito.begin();
  auto pump = [&](int n) {
    for (int i = 0; i < n; ++i)
      vito.loop();
  };
  pump(3);
  io.feed({0x05});  // EOT -> ENQ
  pump(3);
  io.feed({0x06});  // SYNC -> ACK => IDLE
  pump(3);
  io.clear_written();

  if (!vito.rpc(address, parameter.empty() ? nullptr : parameter.data(), static_cast<uint8_t>(parameter.size()))) {
    r.failed = true;
    return r;
  }
  pump(8);
  std::vector<uint8_t> reply{0x06};  // the controller's ACK of the request
  reply.insert(reply.end(), answer.begin(), answer.end());
  io.feed(reply);
  pump(8);
  r.written = io.written();
  return r;
}

int main() {
  std::printf("== VS2 remote procedure call ==\n");
  const std::vector<uint8_t> entry{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

  // A + B: one-byte parameter, answer counted by DataLength.
  {
    std::vector<uint8_t> counted{0x0B, 0x01, 0x07, 0xA0, 0x10, 0x06};
    counted.insert(counted.end(), entry.begin(), entry.end());
    Result r = run(0xA010, {0x05}, frame(counted));
    std::vector<uint8_t> request = frame({0x06, 0x00, 0x07, 0xA0, 0x10, 0x01, 0x05});
    request.push_back(0x06);  // the engine's ACK of the answer
    check(r.written == request, "A: request is 41 06 00 07 A0 10 01 05 <cs>, then ACK");
    check(r.answered && !r.failed, "B: answer with DataLength = returned bytes completes");
    check(r.payload == entry, "B: all six returned bytes are delivered");
    check(r.address == 0xA010, "B: the echoed address is surfaced");
  }

  // C: DataLength repeats the parameter's length; the frame decides.
  {
    std::vector<uint8_t> counted{0x0B, 0x01, 0x07, 0xA0, 0x10, 0x01};
    counted.insert(counted.end(), entry.begin(), entry.end());
    Result r = run(0xA010, {0x05}, frame(counted));
    check(r.answered && !r.failed, "C: answer with DataLength = parameter length completes");
    check(r.payload == entry, "C: the frame's six bytes are delivered, not one");

    std::vector<uint8_t> bad = frame(counted);
    bad.back() = static_cast<uint8_t>(bad.back() + 1);
    auto *parser = new optolink::internals::ParserVS2();
    ParserResult res = ParserResult::CONTINUE;
    for (uint8_t b : bad)
      res = parser->parse(b);
    check(res == ParserResult::CS_ERROR, "C: that shape with a wrong checksum is refused");
    delete parser;
  }

  // D: a procedure without a parameter.
  {
    Result r = run(0xA029, {}, frame({0x05, 0x01, 0x07, 0xA0, 0x29, 0x00}));
    std::vector<uint8_t> request = frame({0x05, 0x00, 0x07, 0xA0, 0x29, 0x00});
    request.push_back(0x06);
    check(r.written == request, "D: no parameter: 41 05 00 07 A0 29 00 <cs>");
    check(r.answered && r.payload.empty(), "D: an empty answer completes with no bytes");
  }

  // E: reads and writes keep their guards.
  {
    optolink::PacketVS2 packet;
    check(!packet.createPacket(optolink::PacketType::REQUEST, optolink::FunctionCode::READ, 0, 0x0800, 0),
          "E: a zero-length READ is still refused");
    check(!packet.createPacket(optolink::PacketType::REQUEST, optolink::FunctionCode::WRITE, 0, 0x2301, 1, nullptr),
          "E: a WRITE without data is still refused");
    const uint8_t param[] = {0x00, 0x01};
    check(packet.createPacket(optolink::PacketType::REQUEST, optolink::FunctionCode::RPC, 0, 0xA043, 2, param),
          "E: a two-byte RPC is built");
    // Stored: the length byte (0x07) and the seven bytes it counts.
    check(packet.length() == 8 && packet.dataLength() == 2 && packet.payloadLength() == 2,
          "E: its frame counts the parameter");
  }

  std::printf("vs2 rpc proof: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
