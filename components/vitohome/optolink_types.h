// Include guards, not #pragma once: ESPHome copies this file into the build
// tree while the optolink library includes it from the component, and a guard
// keeps the two copies from both being compiled into one translation unit.
#ifndef VITOHOME_OPTOLINK_TYPES_H
#define VITOHOME_OPTOLINK_TYPES_H

// What every layer of the Optolink engine shares (design-optolink-layers.md,
// section 3). Request, Result and the timing record follow in stage 3 with
// the exchange that uses them.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome::vitohome::optolink {

// The longest payload a telegram carries, sent or answered. The hub reads at
// most 48 bytes and writes at most 32; error and procedure answers get the
// rest. Every engine buffer is sized from it: a longer request is refused, a
// longer answer fails.
inline constexpr uint8_t MAX_PAYLOAD = 64;

// P300 function codes with a name; any other in 1..0x1F is a generic request.
enum class FunctionCode : uint8_t {
  READ = 0x01,
  WRITE = 0x02,
  RPC = 0x07,
};

// The P300 frame type byte.
enum class PacketType : uint8_t {
  REQUEST = 0x00,
  RESPONSE = 0x01,
  UNACKED = 0x02,
  ERROR = 0x03,
};

// How an operation ended. DEVICE_ERROR is a complete, checksum-valid frame
// that is not a response (the P300 error frame, type 03): the device answered,
// so the link is alive. ERROR is malformed traffic, possibly line noise, and
// proves nothing. CONTINUE is never a result; proof_vs2_guards.cpp uses it as
// "nothing seen yet".
enum class OptolinkResult { CONTINUE, TIMEOUT, LENGTH, NACK, CRC, ERROR, DEVICE_ERROR };

// One telegram as a codec builds it: the bytes a link sends, without the
// link's own (the 01 that answers an ENQ, the P300 ACKs). A P300 telegram
// runs from its 41 to its checksum.
struct Telegram {
  static constexpr std::size_t CAPACITY = MAX_PAYLOAD + 8;
  uint8_t bytes[CAPACITY];
  uint8_t length;
  // Whether a payload rides, so the answer is an acknowledgement.
  bool has_payload;
};

// An open frame of `max` bytes at most, sent as given.
inline bool frame_encode(const uint8_t *bytes, uint8_t length, uint8_t max, Telegram &out) {
  out.length = 0;
  out.has_payload = false;
  if (bytes == nullptr || length == 0 || length > max)
    return false;
  std::memcpy(out.bytes, bytes, length);
  out.length = length;
  return true;
}

}  // namespace esphome::vitohome::optolink

#endif  // VITOHOME_OPTOLINK_TYPES_H
