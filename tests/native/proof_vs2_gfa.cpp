// Proof for the P300 GFA read (FunctionCode::GFA_READ, 0x6B): a read in the
// burner control's address space, built as a READ frame with function code
// 0x6B. Unconfirmed on P300 hardware (it is confirmed over KW, type 0x6B); what
// this pins is that the engine sends the frame it means to and handles every
// plausible answer the way it handles a read's.
//
//   A) The request is 41 05 00 6B 40 50 01 01 -- the read frame with 0x6B as
//      the whole flags byte (message id 0), checksum 0x01.
//   B) An answer echoing 0x6B with DataLength = the returned bytes is
//      delivered like a read's, with the echoed address.
//   C) A two-byte answer is delivered whole.
//   D) A device ERROR frame carrying 0x6B ends as DEVICE_ERROR, not as data.
//   E) An answer with any other flags byte that is no valid function code is
//      refused by the parser (ERROR), not delivered.
//   F) READ frames are unchanged, and a READ whose flags carry a message id is
//      still a READ: only the whole byte 0x6B means GFA.
//   G) The packet guards: no message id with GFA_READ, no zero length; a GFA
//      request frame parses as a payload-less request.
//
// Built by build_and_run_protocols.sh under ASan/UBSan with the VS2 engine.

#include <cstdio>
#include <vector>

#include "fake_optolink.h"
#include "optolink/optolink.h"

namespace optolink = esphome::vitohome::optolink;
using optolink::FunctionCode;
using optolink::PacketType;
using optolink::internals::ParserResult;

static int g_fail = 0;
static void check(bool ok, const char *what) {
  std::printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
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
  optolink::OptolinkResult error = optolink::OptolinkResult::CONTINUE;  // no error seen
  bool failed = false;
  uint16_t address = 0;
};

// One request through a synced engine; `answer` is what the controller sends
// back after its ACK.
static Result run(bool gfa, uint16_t address, uint8_t length, const std::vector<uint8_t> &answer) {
  FakeOptolink io;
  optolink::OptolinkEngine<optolink::P300> vito(&io);
  Result r;
  vito.onResponse([&](const uint8_t *data, uint8_t len, uint16_t addr) {
    if (data)
      r.payload.assign(data, data + len);
    r.answered = true;
    r.address = addr;
  });
  vito.onError([&](optolink::OptolinkResult e, uint16_t) {
    r.failed = true;
    r.error = e;
  });
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

  if (!(gfa ? vito.gfaRead(address, length) : vito.read(address, length))) {
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
  std::printf("== VS2 GFA read ==\n");

  // A + B
  {
    Result r = run(true, 0x4050, 1, frame({0x06, 0x01, 0x6B, 0x40, 0x50, 0x01, 0x24}));
    std::vector<uint8_t> request = frame({0x05, 0x00, 0x6B, 0x40, 0x50, 0x01});
    check(request.back() == 0x01, "A: the request checksum is 0x01");
    request.push_back(0x06);  // the engine's ACK of the answer
    check(r.written == request, "A: request is 41 05 00 6B 40 50 01 01, then ACK");
    check(r.answered && !r.failed, "B: an answer echoing 0x6B completes");
    check(r.payload == std::vector<uint8_t>({0x24}), "B: the returned byte is delivered");
    check(r.address == 0x4050, "B: the echoed address is surfaced");
  }

  // C
  {
    Result r = run(true, 0x408C, 2, frame({0x07, 0x01, 0x6B, 0x40, 0x8C, 0x02, 0x12, 0x34}));
    check(r.answered && r.payload == std::vector<uint8_t>({0x12, 0x34}), "C: a two-byte answer is delivered whole");
  }

  // D
  {
    Result r = run(true, 0x4050, 1, frame({0x06, 0x03, 0x6B, 0x40, 0x50, 0x01, 0x01}));
    check(!r.answered && r.failed && r.error == optolink::OptolinkResult::DEVICE_ERROR,
          "D: an error frame carrying 0x6B is DEVICE_ERROR");
  }

  // E
  {
    Result r = run(true, 0x4050, 1, frame({0x06, 0x01, 0x0B, 0x40, 0x50, 0x01, 0x24}));
    check(!r.answered && r.failed && r.error == optolink::OptolinkResult::ERROR,
          "E: flags 0x0B (0x6B without its top bits) is refused");
  }

  // F
  {
    Result r = run(false, 0x00F8, 2, frame({0x07, 0x01, 0x01, 0x00, 0xF8, 0x02, 0x20, 0xCB}));
    std::vector<uint8_t> request = frame({0x05, 0x00, 0x01, 0x00, 0xF8, 0x02});
    request.push_back(0x06);
    check(r.written == request, "F: a READ is still 41 05 00 01 00 F8 02 <cs>");
    check(r.answered && r.payload == std::vector<uint8_t>({0x20, 0xCB}), "F: and its answer is delivered");

    optolink::PacketVS2 p;
    check(p.createPacket(PacketType::REQUEST, FunctionCode::READ, 3, 0x00F8, 1), "F: a READ with message id 3 builds");
    check(p[2] == 0x61 && p.functionCode() == FunctionCode::READ, "F: flags 0x61 still read back as READ");
  }

  // G
  {
    optolink::PacketVS2 p;
    check(!p.createPacket(PacketType::REQUEST, FunctionCode::GFA_READ, 1, 0x4050, 1),
          "G: GFA_READ with a message id is refused");
    check(!p.createPacket(PacketType::REQUEST, FunctionCode::GFA_READ, 0, 0x4050, 0),
          "G: zero-length GFA_READ is refused");
    check(p.createPacket(PacketType::REQUEST, FunctionCode::GFA_READ, 0, 0x4050, 1) && p[2] == 0x6B &&
              p.functionCode() == FunctionCode::GFA_READ,
          "G: GFA_READ is the whole flags byte 0x6B");

    auto *parser = new optolink::internals::ParserVS2();
    ParserResult res = ParserResult::CONTINUE;
    for (uint8_t b : frame({0x05, 0x00, 0x6B, 0x40, 0x50, 0x01}))
      res = parser->parse(b);
    check(res == ParserResult::COMPLETE && parser->packet().functionCode() == FunctionCode::GFA_READ,
          "G: a GFA request frame parses without a payload");
    delete parser;
  }

  std::printf("vs2 gfa read: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
