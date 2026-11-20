// Include guards, not #pragma once: see optolink_types.h.
#ifndef VITOHOME_OPTOLINK_CODEC_P300_H
#define VITOHOME_OPTOLINK_CODEC_P300_H

// The P300 codec (design section 4.1; openv wiki, Protokoll 300): a request
// as a whole telegram, and the incremental parser of the answer frame.
//
//   41 LL TT FC AH AL DL [payload] CS
//
//   LL  5 + the payload's length: the bytes from TT to the payload's end
//   TT  00 request, 01 response, 03 error
//   FC  the function code in the low five bits (01 read, 02 write, 07 procedure
//       call), the message id, always 0 here, in the high three
//   DL  the data length: a read's answer length, a write's payload length
//   CS  the sum of LL .. the last payload byte, mod 256
//
// A request carries a payload for a write, a procedure call (its parameter,
// possibly none) or any code given one. An answer is a response or an error
// frame; a response to a read carries DL data bytes, to a write none, to a
// procedure call or an unnamed code as many as LL says.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "optolink_types.h"

namespace esphome::vitohome::optolink {

inline constexpr uint8_t P300_START = 0x41;
// LL .. the last payload byte at most: the header and MAX_PAYLOAD bytes.
inline constexpr uint8_t P300_FRAME_MAX = 6 + MAX_PAYLOAD;
static_assert(2u + P300_FRAME_MAX <= Telegram::CAPACITY, "a P300 frame with 41 and CS must fit a Telegram");

// The request 41 .. CS for function 1..0x1F. False for a zero length outside a
// procedure call, a payload past MAX_PAYLOAD, or a write without one.
inline bool p300_encode(uint8_t function, uint16_t address, uint8_t length, const uint8_t *payload, Telegram &out) {
  out.length = 0;
  out.has_payload = false;
  const bool rpc = function == static_cast<uint8_t>(FunctionCode::RPC);
  if (length == 0 && !rpc)
    return false;
  const bool has_payload = function == static_cast<uint8_t>(FunctionCode::WRITE) || rpc || payload != nullptr;
  if (has_payload && payload == nullptr && length != 0)
    return false;
  if (has_payload && length > MAX_PAYLOAD)
    return false;
  uint8_t n = 0;
  out.bytes[n++] = P300_START;
  out.bytes[n++] = static_cast<uint8_t>(has_payload ? 5 + length : 5);
  out.bytes[n++] = static_cast<uint8_t>(PacketType::REQUEST);
  out.bytes[n++] = function;
  out.bytes[n++] = static_cast<uint8_t>(address >> 8);
  out.bytes[n++] = static_cast<uint8_t>(address & 0xFF);
  out.bytes[n++] = length;
  if (has_payload && length != 0) {
    std::memcpy(&out.bytes[n], payload, length);
    n = static_cast<uint8_t>(n + length);
  }
  uint8_t sum = 0;
  for (uint8_t i = 1; i < n; ++i)
    sum = static_cast<uint8_t>(sum + out.bytes[i]);
  out.bytes[n++] = sum;
  out.length = n;
  out.has_payload = has_payload;
  return true;
}

// A request telegram's function code and address.
inline uint8_t p300_function(const Telegram &t) { return t.bytes[3] & 0x1F; }
inline uint16_t p300_address(const Telegram &t) { return static_cast<uint16_t>(t.bytes[4] << 8 | t.bytes[5]); }

// An answer frame as the parser holds it: LL .. the last payload byte, the
// 41 and the checksum not included.
class P300Frame {
 public:
  uint8_t length() const { return static_cast<uint8_t>(buffer_[0] + 1); }
  PacketType packet_type() const { return static_cast<PacketType>(buffer_[1]); }
  uint8_t function_code() const { return buffer_[2] & 0x1F; }
  uint16_t address() const { return static_cast<uint16_t>(buffer_[3] << 8 | buffer_[4]); }
  uint8_t data_length() const { return buffer_[5]; }
  uint8_t payload_length() const { return static_cast<uint8_t>(buffer_[0] - 5); }
  // The data a response delivers: none for a write's.
  const uint8_t *data() const {
    return this->function_code() == static_cast<uint8_t>(FunctionCode::WRITE) ? nullptr : &buffer_[6];
  }
  // The payload bytes whatever the function code: payload_length() of them.
  const uint8_t *payload() const { return &buffer_[6]; }
  uint8_t checksum() const {
    uint8_t sum = 0;
    for (std::size_t i = 0; i <= buffer_[0]; ++i)
      sum = static_cast<uint8_t>(sum + buffer_[i]);
    return sum;
  }

 private:
  friend class P300Parser;
  std::array<uint8_t, P300_FRAME_MAX> buffer_{};
};

enum class P300Parse : uint8_t { CONTINUE, COMPLETE, CS_ERROR, ERROR };

// The answer, one byte at a time. Bytes before a 41 are skipped; a frame that
// cannot be one (LL under 5 or past P300_FRAME_MAX, a type over 03, an
// unnamed function code that was not asked for, a DL that contradicts LL)
// ends in ERROR, a wrong checksum in CS_ERROR, and the parser waits for the
// next 41.
class P300Parser {
 public:
  P300Parse parse(uint8_t b) {
    switch (step_) {
      case Step::START:
        if (b != P300_START)
          break;
        frame_.buffer_[0] = 0;
        step_ = Step::LENGTH;
        break;
      case Step::LENGTH:
        if (b < 5 || static_cast<std::size_t>(b) + 1 > P300_FRAME_MAX)
          return this->fail(P300Parse::ERROR);
        frame_.buffer_[0] = b;
        step_ = Step::TYPE;
        break;
      case Step::TYPE:
        if (b > static_cast<uint8_t>(PacketType::ERROR))
          return this->fail(P300Parse::ERROR);
        frame_.buffer_[1] = b;
        step_ = Step::FLAGS;
        break;
      case Step::FLAGS: {
        const uint8_t function = b & 0x1F;
        if (!named(function) && (expected_function_ == 0 || function != expected_function_))
          return this->fail(P300Parse::ERROR);
        frame_.buffer_[2] = b;
        step_ = Step::ADDRESS_HIGH;
        break;
      }
      case Step::ADDRESS_HIGH:
        frame_.buffer_[3] = b;
        step_ = Step::ADDRESS_LOW;
        break;
      case Step::ADDRESS_LOW:
        frame_.buffer_[4] = b;
        step_ = Step::DATA_LENGTH;
        break;
      case Step::DATA_LENGTH: {
        frame_.buffer_[5] = b;
        const uint8_t function = frame_.function_code();
        const PacketType type = frame_.packet_type();
        if ((function == static_cast<uint8_t>(FunctionCode::READ) && type == PacketType::REQUEST) ||
            (function == static_cast<uint8_t>(FunctionCode::WRITE) && type == PacketType::RESPONSE)) {
          step_ = Step::CHECKSUM;
        } else if ((function == static_cast<uint8_t>(FunctionCode::RPC) && type == PacketType::RESPONSE) ||
                   type == PacketType::ERROR || !named(function)) {
          // Sized by LL: a procedure call's answer, an error frame, an
          // unnamed code's answer.
          remaining_ = frame_.payload_length();
          step_ = remaining_ == 0 ? Step::CHECKSUM : Step::PAYLOAD;
        } else if (b != frame_.length() - 6U) {
          return this->fail(P300Parse::ERROR);
        } else if (b == 0) {
          step_ = Step::CHECKSUM;
        } else {
          remaining_ = b;
          step_ = Step::PAYLOAD;
        }
        break;
      }
      case Step::PAYLOAD:
        frame_.buffer_[6 + frame_.payload_length() - remaining_--] = b;
        if (remaining_ == 0)
          step_ = Step::CHECKSUM;
        break;
      case Step::CHECKSUM:
        if (frame_.checksum() != b)
          return this->fail(P300Parse::CS_ERROR);
        step_ = Step::START;
        return P300Parse::COMPLETE;
    }
    return P300Parse::CONTINUE;
  }

  const P300Frame &frame() const { return frame_; }

  void reset() {
    frame_.buffer_[0] = 0;
    step_ = Step::START;
    remaining_ = 0;
  }

  // Accept a frame with this function code too (0: none beyond the named
  // ones), for the request in flight; its payload is sized by LL. Survives
  // reset(): it belongs to the request, not to one frame.
  void expect_function(uint8_t function) { expected_function_ = function & 0x1F; }

 private:
  enum class Step : uint8_t { START, LENGTH, TYPE, FLAGS, ADDRESS_HIGH, ADDRESS_LOW, DATA_LENGTH, PAYLOAD, CHECKSUM };

  static bool named(uint8_t function) {
    return function == static_cast<uint8_t>(FunctionCode::READ) ||
           function == static_cast<uint8_t>(FunctionCode::WRITE) || function == static_cast<uint8_t>(FunctionCode::RPC);
  }

  P300Parse fail(P300Parse result) {
    step_ = Step::START;
    return result;
  }

  P300Frame frame_;
  Step step_{Step::START};
  uint8_t remaining_{0};
  uint8_t expected_function_{0};
};

}  // namespace esphome::vitohome::optolink

#endif  // VITOHOME_OPTOLINK_CODEC_P300_H
