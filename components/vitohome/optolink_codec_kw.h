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

// The least pause between an answer and a GFA read chained onto it: the read
// is forwarded to the burner control, a second controller. A VScotHO1_72
// answered every 6B sent 25 ms after the previous answer (2026-10-26); without
// the pause it was never tried (THIRD_PARTY.md item 27).
inline constexpr uint8_t KW_GFA_GAP_MS = 25;

// The longest KW telegram or open frame: the header and MAX_PAYLOAD bytes.
inline constexpr uint8_t KW_FRAME_MAX = 4 + MAX_PAYLOAD;
static_assert(KW_FRAME_MAX <= Telegram::CAPACITY, "a KW frame must fit a Telegram");

// False for a zero length or a payload past MAX_PAYLOAD.
inline bool kw_encode(uint8_t type, uint16_t address, uint8_t length, const uint8_t *payload, Telegram &out) {
  out.length = 0;
  out.has_payload = false;
  out.chain_gap_ms = 0;
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
  out.chain_gap_ms = type == PacketVS1Type.GFA_READ ? KW_GFA_GAP_MS : 0;
  return true;
}

// What the exchange needs of a KW request: its telegram, and the answer it
// waits for, `expect` bytes or, with `collect`, whatever arrives until the
// line is quiet.
struct KwCodec {
  static bool prepare(const Request &request, Telegram &telegram, uint8_t &expect, bool &collect) {
    collect = false;
    if (request.lane == Lane::FRAME) {
      if (request.answer_length > MAX_PAYLOAD || !frame_encode(request.payload, request.length, KW_FRAME_MAX, telegram))
        return false;
      expect = request.answer_length;
      collect = request.answer_length == 0;
      return true;
    }
    if (request.lane != Lane::KW)
      return false;
    const uint8_t *payload = request.has_payload ? request.payload : nullptr;
    // A read's answer must fit the link's buffer; a payload is acknowledged
    // with one byte.
    if (payload == nullptr && request.length > MAX_PAYLOAD)
      return false;
    if (!kw_encode(request.code, request.address, request.length, payload, telegram))
      return false;
    expect = payload != nullptr ? 1 : request.length;
    return true;
  }

  // The acknowledgement of a payload is 00; anything else is the device's
  // refusal.
  static OptolinkResult accept(const Telegram &telegram, const uint8_t *answer, uint8_t length) {
    return telegram.has_payload && length != 0 && answer[0] != 0x00 ? OptolinkResult::DEVICE_ERROR : OptolinkResult::OK;
  }
};

}  // namespace esphome::vitohome::optolink

#endif  // VITOHOME_OPTOLINK_CODEC_KW_H
