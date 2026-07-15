// Regression proof for PacketVS2::createPacket's RESPONSE payload guards.
//
// The payload copy loop runs for `fc == WRITE || pt == RESPONSE` (a read
// response echoes the data back), but every guard ahead of it -- the
// null-data check, the len > 250 cap, and the `needed` size computation --
// used to key on `fc == WRITE` alone (the upstream shape). A RESPONSE
// therefore sailed past guards sized for a 6-byte header and then copied
// `len` bytes:
//
//   * RESPONSE with len 251..255: `needed` computed as 6, size check passes,
//     copy writes up to _buffer[260] in a 256-byte std::array -- an
//     out-of-bounds write (stack-buffer-overflow under -fsanitize=address).
//   * RESPONSE with data == nullptr: the WRITE-only mismatch check passes,
//     the copy dereferences null (SEGV under ASan).
//
// Both were LATENT -- the engines only ever build REQUESTs -- but present,
// and one caps-raise away from live. The fix keys every guard on one
// `has_payload` bool matching the copy condition, and gives a payload-bearing
// RESPONSE the protocol-correct length byte (0x05 + len; the P300 length byte
// counts mt, fc, addr(2), len plus the payload it carries -- previously a
// RESPONSE serialized 0x05 regardless).
//
// Pre-fix, scenarios C and D trap under -fsanitize=address; post-fix every
// scenario is a clean accept/reject with the exact bytes asserted below.
// REQUEST construction (the only path the engines use) is pinned unchanged.
//
// Build (from tests/native): compile this TU together with packet_vs2.cpp and
// constants.cpp under -fsanitize=address,undefined with
// -I../../components/vitohome/optolink. See build_and_run.sh.

#include <cstdint>
#include <cstdio>

#include "protocol/vs2/packet_vs2.h"

namespace optolink = esphome::vitohome::optolink;
using optolink::FunctionCode;
using optolink::PacketType;
using optolink::PacketVS2;

static int g_fail = 0;
static void check(bool ok, const char *what) {
  std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_fail;
}

int main() {
  std::printf("== PacketVS2::createPacket RESPONSE payload guards ==\n");

  // A) REQUEST/READ -- the engine's read path, must be byte-identical to the
  //    pre-fix serialization: length byte 0x05, no payload.
  {
    PacketVS2 p;
    check(p.createPacket(PacketType::REQUEST, FunctionCode::READ, 0, 0x5525, 2), "A: REQUEST/READ accepted");
    check(p[0] == 0x05, "A: length byte is 0x05 (no payload)");
    check(p.length() == 6, "A: frame length 6 (length byte + 5)");
    check(p.address() == 0x5525 && p.dataLength() == 2, "A: header fields serialized");
  }

  // B) REQUEST/WRITE -- the engine's write path, also byte-identical.
  {
    PacketVS2 p;
    const uint8_t payload[2] = {0xAA, 0xBB};
    check(p.createPacket(PacketType::REQUEST, FunctionCode::WRITE, 0, 0x2306, 2, payload), "B: REQUEST/WRITE accepted");
    check(p[0] == 0x07, "B: length byte is 0x05 + len");
    check(p[6] == 0xAA && p[7] == 0xBB, "B: payload copied");
  }

  // C) RESPONSE with len 255 and a valid 255-byte source. Pre-fix: needed
  //    computed as 6, size check passes, the copy writes _buffer[6..260] --
  //    OOB past the 256-byte array (ASan stack-buffer-overflow). Post-fix:
  //    rejected by the len > 250 payload cap before any byte moves.
  {
    PacketVS2 p;
    uint8_t big[255];
    for (unsigned i = 0; i < sizeof(big); ++i)
      big[i] = static_cast<uint8_t>(i);
    check(!p.createPacket(PacketType::RESPONSE, FunctionCode::READ, 0, 0x00F8, 255, big),
          "C: RESPONSE len 255 rejected (was OOB write)");
    check(!p.createPacket(PacketType::RESPONSE, FunctionCode::READ, 0, 0x00F8, 251, big),
          "C: RESPONSE len 251 rejected (first bad length)");
  }

  // D) RESPONSE with a null data pointer. Pre-fix: the mismatch check only
  //    guarded WRITE, so the copy dereferenced null. Post-fix: rejected.
  {
    PacketVS2 p;
    check(!p.createPacket(PacketType::RESPONSE, FunctionCode::READ, 0, 0x00F8, 4, nullptr),
          "D: RESPONSE with null data rejected (was null deref)");
  }

  // E) Valid RESPONSE: payload rides, and the length byte counts it.
  {
    PacketVS2 p;
    const uint8_t payload[4] = {0x01, 0x02, 0x03, 0x04};
    check(p.createPacket(PacketType::RESPONSE, FunctionCode::READ, 0, 0x00F8, 4, payload),
          "E: RESPONSE len 4 accepted");
    check(p[0] == 0x09, "E: length byte is 0x05 + len");
    check(p[6] == 0x01 && p[9] == 0x04, "E: payload copied");
    check(p.dataLength() == 4 && p.data() != nullptr && p.data()[0] == 0x01, "E: accessors see the payload");
  }

  // F) Boundary: len 250 is the largest payload; needed == 256 fills the
  //    array exactly, with the last payload byte at _buffer[255].
  {
    PacketVS2 p;
    uint8_t big[250];
    for (unsigned i = 0; i < sizeof(big); ++i)
      big[i] = static_cast<uint8_t>(i);
    check(p.createPacket(PacketType::RESPONSE, FunctionCode::READ, 0, 0x00F8, 250, big),
          "F: RESPONSE len 250 fills the buffer exactly");
    check(p[0] == 0xFF && p[255] == 249, "F: length byte 0xFF, last byte in the last slot");
  }

  if (g_fail == 0) {
    std::printf("proof_packet_vs2_response: all checks passed\n");
    return 0;
  }
  std::printf("proof_packet_vs2_response: %d FAILURES\n", g_fail);
  return 1;
}
