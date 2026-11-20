// Include guards, not #pragma once: see optolink_types.h.
#ifndef VITOHOME_OPTOLINK_CODEC_KW_H
#define VITOHOME_OPTOLINK_CODEC_KW_H

// The KW codec (design section 4.2; openv wiki, Protokoll KW): the bytes of
// one telegram, without the 01 a link puts in front when it answers an ENQ.
//
//   TT AH AL LEN [payload]
//
//   TT  F7 read, F4 write (LEN payload bytes follow), 6B a read of the burner
//       control (GFA), any other type as given
//
// The answer is LEN raw bytes, or after a payload one byte, 00 for success.
// An open frame is sent as given, its first byte included.

#include <cstdint>
#include <cstring>

#include "optolink_types.h"

namespace esphome::vitohome::optolink {

// The KW TYPE bytes with a name. 6B reads the burner control (GFA) in its own
// address space, a KW telegram only: a VScotHO1_72 refuses it as a P300
// function code (error 05), so the P300 engine reads it over the KW detour
// (THIRD_PARTY.md item 27).
constexpr struct {
  uint8_t READ = 0xF7;
  uint8_t WRITE = 0xF4;
  uint8_t GFA_READ = 0x6B;
} PacketVS1Type;

// The longest KW telegram or open frame: the header and MAX_PAYLOAD bytes.
inline constexpr uint8_t KW_FRAME_MAX = 4 + MAX_PAYLOAD;
static_assert(KW_FRAME_MAX <= Telegram::CAPACITY, "a KW frame must fit a Telegram");

// False for a zero length or a payload past MAX_PAYLOAD.
inline bool kw_encode(uint8_t type, uint16_t address, uint8_t length, const uint8_t *payload, Telegram &out) {
  out.length = 0;
  out.has_payload = false;
  if (length == 0 || (payload != nullptr && 4u + length > KW_FRAME_MAX))
    return false;
  uint8_t n = 0;
  out.bytes[n++] = type;
  out.bytes[n++] = static_cast<uint8_t>(address >> 8);
  out.bytes[n++] = static_cast<uint8_t>(address & 0xFF);
  out.bytes[n++] = length;
  if (payload != nullptr) {
    std::memcpy(&out.bytes[n], payload, length);
    n = static_cast<uint8_t>(n + length);
  }
  out.length = n;
  out.has_payload = payload != nullptr;
  return true;
}

}  // namespace esphome::vitohome::optolink

#endif  // VITOHOME_OPTOLINK_CODEC_KW_H
