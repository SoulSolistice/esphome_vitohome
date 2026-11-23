// Include guards, not #pragma once: ESPHome copies this file into the build
// tree while the optolink library includes it from the component, and a guard
// keeps the two copies from both being compiled into one translation unit.
#ifndef VITOHOME_OPTOLINK_TYPES_H
#define VITOHOME_OPTOLINK_TYPES_H

// What every layer of the Optolink engine shares (design-optolink-layers.md,
// section 3).

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
// "nothing seen yet". OK comes last so the error codes keep their values
// while the old engines still report them.
enum class OptolinkResult { CONTINUE, TIMEOUT, LENGTH, NACK, CRC, ERROR, DEVICE_ERROR, OK };

// One telegram as a codec builds it: the bytes a link sends, without the
// link's own (the 01 that answers an ENQ, the P300 ACKs). A P300 telegram
// runs from its 41 to its checksum.
struct Telegram {
  static constexpr std::size_t CAPACITY = MAX_PAYLOAD + 8;
  uint8_t bytes[CAPACITY];
  uint8_t length;
  // Whether a payload rides, so the answer is an acknowledgement.
  bool has_payload;
  // The least pause after the previous answer when chained (KW 6B: 25 ms).
  uint8_t chain_gap_ms;
};

// The moments a link holds a write back (design-optolink-layers.md, section
// 13), each settable up to its bound. KW has the first two, P300 the first
// four, GWG ENQ_TO_SEND and ACK_TO_TELEGRAM.
enum class TimingPoint : uint8_t {
  // The device's ENQ read -> the 01 and the telegram; P300: on the KW
  // detour.
  ENQ_TO_SEND,
  // KW: an answer -> the next telegram chained onto it; a GFA read keeps at
  // least its own 25 ms. P300: our ACK of an answer -> the next telegram.
  ANSWER_TO_NEXT,
  // P300: the device's ENQ after our EOT -> our 16 00 00.
  ENQ_TO_SYNC,
  // P300: the device's answer frame complete -> our ACK.
  FRAME_TO_ACK,
  // GWG: the 01 that answers the device's ENQ -> the rest of the telegram.
  ACK_TO_TELEGRAM,
};
inline constexpr uint8_t TIMING_POINTS = 5;

// The bounds: well inside what a VScotHO1_72 takes (200 ms after its KW
// ENQ, 0.91 s after a KW answer) and inside the KW link's 500 ms chain
// window. Together far inside the 4 s watchdog of a request.
inline constexpr uint32_t ENQ_TO_SEND_MAX_MS = 100;
inline constexpr uint32_t ANSWER_TO_NEXT_MAX_MS = 400;
inline constexpr uint32_t ENQ_TO_SYNC_MAX_MS = 100;
inline constexpr uint32_t FRAME_TO_ACK_MAX_MS = 400;
inline constexpr uint32_t ACK_TO_TELEGRAM_MAX_MS = 100;

constexpr uint32_t timing_max(TimingPoint point) {
  switch (point) {
    case TimingPoint::ENQ_TO_SEND:
      return ENQ_TO_SEND_MAX_MS;
    case TimingPoint::ANSWER_TO_NEXT:
      return ANSWER_TO_NEXT_MAX_MS;
    case TimingPoint::ENQ_TO_SYNC:
      return ENQ_TO_SYNC_MAX_MS;
    case TimingPoint::ACK_TO_TELEGRAM:
      return ACK_TO_TELEGRAM_MAX_MS;
    case TimingPoint::FRAME_TO_ACK:
    default:
      return FRAME_TO_ACK_MAX_MS;
  }
}

constexpr const char *timing_name(TimingPoint point) {
  switch (point) {
    case TimingPoint::ENQ_TO_SEND:
      return "ENQ_TO_SEND";
    case TimingPoint::ANSWER_TO_NEXT:
      return "ANSWER_TO_NEXT";
    case TimingPoint::ENQ_TO_SYNC:
      return "ENQ_TO_SYNC";
    case TimingPoint::ACK_TO_TELEGRAM:
      return "ACK_TO_TELEGRAM";
    case TimingPoint::FRAME_TO_ACK:
    default:
      return "FRAME_TO_ACK";
  }
}

// How long a link waits for something (design-optolink-layers.md, section
// 13.1), settable within its link's bounds. GWG has the answer deadline.
enum class LinkWindow : uint8_t {
  // The silence after the telegram, or after an answer's latest byte, that
  // ends the wait for its answer.
  ANSWER_DEADLINE,
};
inline constexpr uint8_t LINK_WINDOWS = 1;

constexpr const char *window_name(LinkWindow window) {
  switch (window) {
    case LinkWindow::ANSWER_DEADLINE:
    default:
      return "ANSWER_DEADLINE";
  }
}

// Which telegram a request is: a protocol's own, or an open frame sent as
// given.
enum class Lane : uint8_t { P300, KW, GWG, FRAME };

// One operation as the hub asks for it. The exchange copies it on submit.
struct Request {
  Lane lane;
  // The P300 function code, the KW or GWG TYPE byte; unused for FRAME.
  uint8_t code;
  // GWG 0..0xFF; FRAME 0.
  uint16_t address;
  // A read's answer length, a payload's length; FRAME: the frame's length.
  uint8_t length;
  bool has_payload;
  // The payload, or FRAME's bytes.
  uint8_t payload[Telegram::CAPACITY];
  // FRAME only: the answer's length, 0 for until the line is quiet.
  uint8_t answer_length;
};

// A protocol's telegram; `payload` (null: none) copied up to the capacity,
// the codec refusing a length past what it takes.
inline Request make_request(Lane lane, uint8_t code, uint16_t address, uint8_t length, const uint8_t *payload) {
  Request r{};
  r.lane = lane;
  r.code = code;
  r.address = address;
  r.length = length;
  r.has_payload = payload != nullptr;
  if (payload != nullptr)
    std::memcpy(r.payload, payload, length < sizeof(r.payload) ? length : sizeof(r.payload));
  return r;
}

// An open frame: `length` bytes sent as given, `answer_length` answer bytes
// (0: until the line is quiet).
inline Request make_frame(const uint8_t *bytes, uint8_t length, uint8_t answer_length) {
  Request r = make_request(Lane::FRAME, 0, 0, length, bytes);
  r.has_payload = false;
  r.answer_length = answer_length;
  if (bytes == nullptr)
    r.length = 0;
  return r;
}

// How a telegram went out on an ENQ-paced line. Only the GWG engine measures
// it so far; on the other links it stays zero.
struct Timing {
  // Sent on an ENQ; false when burst-sent.
  bool enq_paced;
  // The loop gap of the pass that read the ENQ: an upper bound on its age.
  uint32_t enq_age_ms;
  uint32_t enq_to_send_ms;
  // The 01 to the rest of the telegram.
  uint32_t ack_gap_ms;
  // Whether send_to_answer_ms holds the time from the request's last byte to
  // a read's complete answer.
  bool answer_timed;
  uint32_t send_to_answer_ms;
};

// How a request ended, as the exchange reports it. The pointers are valid
// inside the result callback only.
struct Result {
  OptolinkResult status;
  // P300: the address in the answer; otherwise the request's (FRAME: 0).
  uint16_t address;
  // The answer; null on an error.
  const uint8_t *data;
  uint8_t length;
  // DEVICE_ERROR on P300: the error frame's payload, 01 for an address the
  // controller does not serve, 05 for a function code it lacks.
  const uint8_t *error_code;
  uint8_t error_code_length;
  // GWG: a TIMEOUT before any ENQ came, so the request never went out.
  bool never_sent;
  // A GWG EEPROM cell answered FF FF (not in use), delivered as FF.
  bool unserved_eeprom;
  // How often the telegram went out again after silence (KW, and KW over the
  // P300 detour).
  uint8_t retries;
  Timing timing;
};

// An open frame of `max` bytes at most, sent as given.
inline bool frame_encode(const uint8_t *bytes, uint8_t length, uint8_t max, Telegram &out) {
  out.length = 0;
  out.has_payload = false;
  out.chain_gap_ms = 0;
  if (bytes == nullptr || length == 0 || length > max)
    return false;
  std::memcpy(out.bytes, bytes, length);
  out.length = length;
  out.chain_gap_ms = 0;
  return true;
}

}  // namespace esphome::vitohome::optolink

#endif  // VITOHOME_OPTOLINK_TYPES_H
