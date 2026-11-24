// Include guards, not #pragma once: see optolink_types.h.
#ifndef VITOHOME_OPTOLINK_P300_H
#define VITOHOME_OPTOLINK_P300_H

// The P300 stack (design-optolink-layers.md, sections 4.1 and 5.2): the
// session with the device and its keepalive, one telegram at a time with the
// device's ACK and its answer frame, the KW detour for a KW telegram, and the
// open frame; one request in flight, its watchdog and one result callback,
// as the KW exchange has them (optolink_exchange.h).
//
//   RESET        the parser reset, the line drained; once it has been quiet
//                for QuietMs after a byte other than the 05, 04 -> RESET_ACK
//   RESET_ACK    05 -> INIT, or a KW telegram pending goes out on it; 15 ->
//                RESET; 3 s without -> RESET
//   INIT         16 00 00, ENQ_TO_SYNC after the 05 -> INIT_ACK
//   INIT_ACK     06 -> IDLE; any other byte, or 3 s -> RESET
//   IDLE         3 s since the last activity -> INIT (the keepalive); a KW
//                telegram -> KW_DROP, an open frame -> FRAME_SEND, a
//                telegram -> the line drained, 41 .. CS in this pass;
//                ANSWER_TO_NEXT after our 06
//   SEND_ACK     one byte per pass: 06 -> RECEIVE, 15 -> IDLE and NACK
//   RECEIVE      the answer through the parser (P300Parser)
//   RECEIVE_ACK  06, FRAME_TO_ACK after the frame -> IDLE
//   KW_DROP      as RESET, 04 -> KW_ENQ
//   KW_ENQ       05 -> 01 and the telegram in this pass, or KW_HOLD; 15 ->
//                KW_DROP; 3 s without -> RESET and TIMEOUT
//   KW_HOLD      the line drained; ENQ_TO_SEND after the 05 -> 01 and the
//                telegram
//   KW_RECEIVE   <len> bytes, or one acknowledgement byte after a payload;
//                1 s of silence, or nothing but an ENQ, checked before the
//                drain -> KW_ENQ for the telegram again, SilenceRetries
//                times, then RESET and TIMEOUT; 1 s after a partial answer
//                -> RESET and TIMEOUT
//   KW_CHAIN     the next KW telegram (a GFA read 25 ms after the answer), a
//                05 opens it or the handshake; any other byte, other work or
//                250 ms -> RESET
//   FRAME_SEND, FRAME_RECEIVE  the bytes as given, the answer raw until its
//                length or 500 ms of quiet -> RESET
//   watchdog     4 s busy, from the submit, the first KW drop, each KW
//                telegram, its retry or an open frame's send -> RESET and
//                TIMEOUT
//
// After its NACK a VScotHO1_72 refuses the next byte it gets, an EOT too:
// 15 about 258 ms later. The byte after that it takes (2026-11-24, an EOT
// 1-255 ms after a NACKed timed frame, and the telegrams after a NACK).
// The EOT goes out again once that 15 is past, instead of 3 s later; a
// request waiting meanwhile would run into its watchdog.
//
// The timing points (TimingPoint) are 0 by default: the pacing without them,
// the next pass for ENQ_TO_SYNC, FRAME_TO_ACK and ANSWER_TO_NEXT, the ENQ's
// own pass for ENQ_TO_SEND. A held write goes out in the first pass at or
// after its moment, and the loop runs fast meanwhile.
//
// The request stays busy during its result callback, so a submit from inside
// it is refused.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>

#include "optolink_clock.h"
#include "optolink_codec_kw.h"
#include "optolink_codec_p300.h"
#include "optolink_types.h"

namespace esphome::vitohome::optolink {

inline constexpr uint32_t P300_WATCHDOG_MS = 4000;
inline constexpr uint32_t P300_HANDSHAKE_RETRY_MS = 3000;
inline constexpr uint32_t P300_KEEPALIVE_MS = 3000;
inline constexpr uint32_t P300_KW_ENQ_TIMEOUT_MS = 3000;
inline constexpr uint32_t P300_KW_LINGER_MS = 250;
inline constexpr uint32_t P300_KW_ANSWER_TIMEOUT_MS = 1000;
inline constexpr uint32_t P300_FRAME_QUIET_MS = 500;
// An open frame and its answer; a KW telegram over the detour.
inline constexpr uint8_t P300_OPEN_FRAME_MAX = MAX_PAYLOAD;
inline constexpr uint8_t P300_KW_MAX_PAYLOAD = 32;
inline constexpr uint8_t P300_KW_MAX_LENGTH = MAX_PAYLOAD;
// How often a KW telegram met by silence goes out again, on the next ENQ.
inline constexpr uint8_t P300_KW_SILENCE_RETRIES = 1;
// The error frame's payload kept for Result::error_code.
inline constexpr uint8_t P300_ERROR_CODE_MAX = 4;
// The quiet before an EOT after the device's bytes: past its late 15.
inline constexpr uint32_t P300_QUIET_MS = 300;

// `QuietMs` 0: the EOT at once and a 15 ignored, as VS2Engine had it.
template<class Port, uint8_t SilenceRetries = P300_KW_SILENCE_RETRIES, uint32_t QuietMs = P300_QUIET_MS>
class P300Exchange {
 public:
  using ResultCallback = std::function<void(const Result &)>;

  explicit P300Exchange(Port *port) : port_(port), now_(optolink_millis()), last_ms_(now_) {}
  P300Exchange(const P300Exchange &) = delete;
  P300Exchange &operator=(const P300Exchange &) = delete;

  void on_result(ResultCallback callback) { on_result_ = std::move(callback); }

  // A restart takes the clock afresh, as the KW exchange does.
  bool begin() {
    now_ = optolink_millis();
    this->set_state_(State::RESET);
    return port_->begin();
  }
  void end() {
    port_->end();
    this->set_state_(State::UNDEFINED);
    busy_ = false;
  }
  bool busy() const { return busy_; }
  // Whether a write waits on a timing point; in IDLE only with a request.
  bool wants_fast_loop() const { return waiting_ && (state_ != State::IDLE || busy_); }

  // A timing point, `ms` clamped to its bound; false for GWG's ACK_TO_TELEGRAM.
  bool set_timing(TimingPoint point, uint32_t &ms) {
    if (point == TimingPoint::ACK_TO_TELEGRAM)
      return false;
    if (ms > timing_max(point))
      ms = timing_max(point);
    timing_[static_cast<uint8_t>(point)] = ms;
    return true;
  }
  uint32_t timing(TimingPoint point) const { return timing_[static_cast<uint8_t>(point)]; }
  uint32_t timing_default(TimingPoint) const { return 0; }
  // No settable window yet.
  bool set_window(LinkWindow, uint32_t &) { return false; }
  uint32_t window(LinkWindow) const { return 0; }
  uint32_t window_default(LinkWindow) const { return 0; }

  // A P300 telegram by function code, a KW telegram over the detour, or an
  // open frame; false while busy or when the request cannot be one.
  bool submit(const Request &request) {
    if (busy_)
      return false;
    const uint8_t *payload = request.has_payload ? request.payload : nullptr;
    switch (request.lane) {
      case Lane::P300:
        return this->request_(request.code, request.address, request.length, payload);
      case Lane::KW:
        return this->kw_request_(request.code, request.address, request.length, payload);
      case Lane::FRAME:
        return this->frame_request_(request.payload, request.length, request.answer_length);
      case Lane::GWG:
      default:
        return false;
    }
  }

  void loop() {
    now_ = optolink_millis();
    switch (state_) {
      case State::RESET:
        this->reset_();
        break;
      case State::RESET_ACK:
        this->reset_ack_();
        break;
      case State::INIT:
        this->init_();
        break;
      case State::INIT_ACK:
        this->init_ack_();
        break;
      case State::IDLE:
        this->idle_();
        break;
      case State::SEND_START:
        this->send_start_();
        break;
      case State::SEND_PACKET:
        this->send_packet_();
        break;
      case State::SEND_CRC:
        this->send_crc_();
        break;
      case State::SEND_ACK:
        this->send_ack_();
        break;
      case State::RECEIVE:
        this->receive_();
        break;
      case State::RECEIVE_ACK:
        this->receive_ack_();
        break;
      case State::KW_DROP:
        this->kw_drop_();
        break;
      case State::KW_ENQ:
        this->kw_enq_();
        break;
      case State::KW_HOLD:
        this->kw_hold_();
        break;
      case State::KW_SEND:
        this->kw_send_();
        break;
      case State::KW_RECEIVE:
        this->kw_receive_();
        break;
      case State::KW_CHAIN:
        this->kw_chain_();
        break;
      case State::FRAME_SEND:
        this->frame_send_();
        break;
      case State::FRAME_RECEIVE:
        this->frame_receive_();
        break;
      case State::UNDEFINED:
        break;
    }
    if (busy_ && now_ - request_ms_ > P300_WATCHDOG_MS) {
      this->set_state_(State::RESET);
      this->fail_(OptolinkResult::TIMEOUT);
    }
  }

 private:
  enum class State : uint8_t {
    UNDEFINED,
    RESET,
    RESET_ACK,
    INIT,
    INIT_ACK,
    IDLE,
    SEND_START,
    SEND_PACKET,
    SEND_CRC,
    SEND_ACK,
    RECEIVE,
    RECEIVE_ACK,
    KW_DROP,
    KW_ENQ,
    KW_HOLD,
    KW_SEND,
    KW_RECEIVE,
    KW_CHAIN,
    FRAME_SEND,
    FRAME_RECEIVE,
  };

  static constexpr uint8_t ENQ = 0x05;
  static constexpr uint8_t ENQ_ACK = 0x01;
  static constexpr uint8_t EOT = 0x04;
  static constexpr uint8_t ACK = 0x06;
  static constexpr uint8_t NACK = 0x15;
  static constexpr uint8_t SYNC[3] = {0x16, 0x00, 0x00};

  bool request_(uint8_t function, uint16_t address, uint8_t length, const uint8_t *data) {
    // The flags byte holds the function code in its low five bits.
    if (function == 0 || function > 0x1F)
      return false;
    // Without a payload `length` is the DataLength the answer echoes, and the
    // answer must fit the parser's frame.
    if (data == nullptr && length > MAX_PAYLOAD)
      return false;
    if (!p300_encode(function, address, length, data, telegram_))
      return false;
    this->start_(address, length);
    parser_.expect_function(function);
    return true;
  }

  bool kw_request_(uint8_t type, uint16_t address, uint8_t length, const uint8_t *data) {
    if (length == 0 || length > (data != nullptr ? P300_KW_MAX_PAYLOAD : P300_KW_MAX_LENGTH))
      return false;
    kw_type_ = type;
    kw_has_payload_ = data != nullptr;
    if (data != nullptr)
      std::memcpy(kw_payload_, data, length);
    this->start_(address, length);
    kw_ = true;
    kw_retries_left_ = SilenceRetries;
    return true;
  }

  bool frame_request_(const uint8_t *bytes, uint8_t length, uint8_t answer_length) {
    if (bytes == nullptr || length == 0 || length > P300_OPEN_FRAME_MAX || answer_length > P300_OPEN_FRAME_MAX)
      return false;
    std::memcpy(frame_bytes_, bytes, length);
    frame_len_ = length;
    this->start_(0, answer_length);
    frame_ = true;
    return true;
  }

  // The watchdog runs from the last pass's clock.
  void start_(uint16_t address, uint8_t length) {
    address_ = address;
    length_ = length;
    kw_ = false;
    frame_ = false;
    retries_ = 0;
    error_len_ = 0;
    kw_dropped_ = false;
    busy_ = true;
    request_ms_ = now_;
  }

  // Every state that counts bytes starts at 0, and without a held write.
  void set_state_(State state) {
    state_ = state;
    count_ = 0;
    hold_ms_ = 0;
    waiting_ = false;
  }

  // The state's write held back by `point` from now.
  void hold_(TimingPoint point) {
    hold_from_ms_ = now_;
    hold_ms_ = timing_[static_cast<uint8_t>(point)];
    waiting_ = hold_ms_ != 0;
  }
  // Whether the held write must wait still.
  bool held_() {
    waiting_ = now_ - hold_from_ms_ < hold_ms_;
    return waiting_;
  }

  void drain_() {
    while (port_->available())
      port_->read();
  }

  // A byte from the device the next EOT must wait out.
  void note_heard_() {
    heard_ms_ = now_;
    heard_ = QuietMs != 0;
  }

  // The line drained; whether it has not been quiet for QuietMs since a byte
  // other than the device's 05.
  bool line_busy_() {
    while (port_->available()) {
      if (port_->read() != ENQ)
        this->note_heard_();
    }
    if (heard_ && now_ - heard_ms_ < QuietMs)
      return true;
    heard_ = false;
    return false;
  }

  void reset_() {
    parser_.reset();
    if (this->line_busy_())
      return;
    if (port_->write(&EOT, 1) == 1) {
      last_ms_ = now_;
      this->set_state_(State::RESET_ACK);
    }
  }

  void reset_ack_() {
    if (port_->available()) {
      const uint8_t b = port_->read();
      if (b == NACK && QuietMs != 0) {
        this->note_heard_();
        this->set_state_(State::RESET);
        return;
      }
      if (b != ENQ)
        return;
      last_ms_ = now_;
      // After the EOT the device speaks KW: a KW telegram waiting goes out on
      // this ENQ, not after a handshake the detour would drop again.
      if (busy_ && kw_) {
        this->kw_enq_read_();
        return;
      }
      this->set_state_(State::INIT);
      this->hold_(TimingPoint::ENQ_TO_SYNC);
    } else if (now_ - last_ms_ > P300_HANDSHAKE_RETRY_MS) {
      this->set_state_(State::RESET);
    }
  }

  void init_() {
    if (count_ == 0 && this->held_())
      return;
    count_ = static_cast<uint8_t>(count_ + port_->write(&SYNC[count_], static_cast<uint8_t>(sizeof(SYNC) - count_)));
    if (count_ == sizeof(SYNC)) {
      last_ms_ = now_;
      this->set_state_(State::INIT_ACK);
    }
  }

  void init_ack_() {
    if (port_->available()) {
      if (port_->read() == ACK) {
        this->set_state_(State::IDLE);
        return;
      }
      this->note_heard_();
      this->set_state_(State::RESET);
    } else if (now_ - last_ms_ > P300_HANDSHAKE_RETRY_MS) {
      this->set_state_(State::RESET);
    }
  }

  void idle_() {
    // Nothing waits without a request.
    if (!busy_) {
      waiting_ = false;
    } else if (this->held_()) {
      return;
    }
    const bool keepalive = now_ - last_ms_ > P300_KEEPALIVE_MS;
    if (busy_) {
      if (kw_) {
        this->set_state_(State::KW_DROP);
        return;
      }
      if (frame_) {
        this->set_state_(State::FRAME_SEND);
        return;
      }
      if (!keepalive) {
        // A byte left on the line would be read as the device's ACK or the
        // start of its frame. The whole telegram goes out in this pass.
        this->drain_();
        this->set_state_(State::SEND_START);
        this->send_start_();
        return;
      }
    }
    if (keepalive)
      this->set_state_(State::INIT);
  }

  // A partial write stays in its state and resumes on the next pass.
  void send_start_() {
    if (port_->write(&P300_START, 1) == 1) {
      last_ms_ = now_;
      this->set_state_(State::SEND_PACKET);
      this->send_packet_();
    }
  }

  void send_packet_() {
    // LL .. the last payload byte: the telegram without its 41 and checksum.
    const auto body = static_cast<uint8_t>(telegram_.length - 2);
    count_ =
        static_cast<uint8_t>(count_ + port_->write(&telegram_.bytes[1 + count_], static_cast<uint8_t>(body - count_)));
    if (count_ == body) {
      last_ms_ = now_;
      this->set_state_(State::SEND_CRC);
      this->send_crc_();
    }
  }

  void send_crc_() {
    if (port_->write(&telegram_.bytes[telegram_.length - 1], 1) == 1) {
      last_ms_ = now_;
      this->set_state_(State::SEND_ACK);
    }
  }

  void send_ack_() {
    if (!port_->available())
      return;
    const uint8_t b = port_->read();
    if (b == ACK) {
      this->set_state_(State::RECEIVE);
    } else if (b == NACK) {
      this->set_state_(State::IDLE);
      this->fail_(OptolinkResult::NACK);
    }
  }

  void receive_() {
    while (port_->available()) {
      last_ms_ = now_;
      const P300Parse parsed = parser_.parse(port_->read());
      if (parsed == P300Parse::COMPLETE) {
        this->answered_();
        return;
      }
      if (parsed == P300Parse::CS_ERROR || parsed == P300Parse::ERROR) {
        // The rest of a broken frame may follow.
        this->note_heard_();
        this->set_state_(State::RESET);
        this->fail_(parsed == P300Parse::CS_ERROR ? OptolinkResult::CRC : OptolinkResult::ERROR);
        return;
      }
    }
  }

  // A complete frame (section 4.1, accept()).
  void answered_() {
    const P300Frame &answer = parser_.frame();
    const uint8_t fc = answer.function_code();
    const bool response = answer.packet_type() == PacketType::RESPONSE;
    const bool read_or_write =
        fc == static_cast<uint8_t>(FunctionCode::READ) || fc == static_cast<uint8_t>(FunctionCode::WRITE);
    if (response && fc == p300_function(telegram_) && read_or_write && answer.address() != p300_address(telegram_)) {
      // An earlier telegram's answer: the stream is a frame behind. RESET
      // drains the line and syncs again.
      this->set_state_(State::RESET);
      this->fail_(OptolinkResult::ERROR);
      return;
    }
    // Every complete frame is acknowledged. A response to another function
    // code is ERROR, the error frame DEVICE_ERROR with its payload.
    this->set_state_(State::RECEIVE_ACK);
    this->hold_(TimingPoint::FRAME_TO_ACK);
    if (response && fc != p300_function(telegram_)) {
      this->fail_(OptolinkResult::ERROR);
    } else if (response) {
      // A write's acknowledgement has no data but a DL; a procedure call's
      // answer, and an unnamed code's, is as long as its frame.
      const uint8_t length = read_or_write ? answer.data_length() : answer.payload_length();
      this->deliver_(answer.data(), length, answer.address());
    } else {
      error_len_ = answer.payload_length() < P300_ERROR_CODE_MAX ? answer.payload_length() : P300_ERROR_CODE_MAX;
      std::memcpy(error_code_, answer.payload(), error_len_);
      this->fail_(OptolinkResult::DEVICE_ERROR);
    }
  }

  void receive_ack_() {
    if (this->held_())
      return;
    if (port_->write(&ACK, 1) == 1) {
      last_ms_ = now_;
      this->set_state_(State::IDLE);
      this->hold_(TimingPoint::ANSWER_TO_NEXT);
    }
  }

  // --- the KW detour: leave P300 with an EOT, the telegram on the ENQ -------

  void kw_drop_() {
    parser_.reset();
    if (this->line_busy_())
      return;
    if (port_->write(&EOT, 1) == 1) {
      last_ms_ = now_;
      // The request may have waited out a handshake: the watchdog runs from
      // the first drop, not from one the device refused.
      if (!kw_dropped_)
        request_ms_ = now_;
      kw_dropped_ = true;
      this->set_state_(State::KW_ENQ);
    }
  }

  void kw_enq_() {
    if (port_->available()) {
      // A 15 refuses the EOT; anything else but the ENQ is left over from
      // P300 and dropped.
      const uint8_t b = port_->read();
      if (b == NACK && QuietMs != 0) {
        this->note_heard_();
        this->set_state_(State::KW_DROP);
      } else if (b == ENQ) {
        this->kw_enq_read_();
      }
    } else if (now_ - last_ms_ > P300_KW_ENQ_TIMEOUT_MS) {
      this->set_state_(State::RESET);
      this->fail_(OptolinkResult::TIMEOUT);
    }
  }

  // The device's ENQ for the KW telegram: answered in this pass, or held.
  void kw_enq_read_() {
    if (timing_[static_cast<uint8_t>(TimingPoint::ENQ_TO_SEND)] == 0) {
      this->kw_start_(true);
      return;
    }
    this->set_state_(State::KW_HOLD);
    this->hold_(TimingPoint::ENQ_TO_SEND);
  }

  // What the device sends meanwhile is no answer.
  void kw_hold_() {
    this->drain_();
    if (!this->held_())
      this->kw_start_(true);
  }

  // The KW telegram, with the 01 in front when it answers an ENQ, sent in the
  // pass that allows it.
  void kw_start_(bool enq_ack) {
    uint8_t n = 0;
    if (enq_ack)
      kw_frame_[n++] = ENQ_ACK;
    Telegram telegram;
    kw_encode(kw_type_, address_, length_, kw_has_payload_ ? kw_payload_ : nullptr, telegram);
    std::memcpy(&kw_frame_[n], telegram.bytes, telegram.length);
    kw_frame_len_ = static_cast<uint8_t>(n + telegram.length);
    request_ms_ = now_;
    this->set_state_(State::KW_SEND);
    this->kw_send_();
  }

  void kw_send_() {
    count_ =
        static_cast<uint8_t>(count_ + port_->write(&kw_frame_[count_], static_cast<uint8_t>(kw_frame_len_ - count_)));
    if (count_ == kw_frame_len_) {
      last_ms_ = now_;
      this->set_state_(State::KW_RECEIVE);
    }
  }

  void kw_receive_() {
    const uint8_t expected = kw_has_payload_ ? 1 : length_;
    // An ignored telegram's next byte is the device's ENQ about 2 s later:
    // checked before the drain, so it is not taken for data. An unheard
    // telegram goes out again on the ENQ after that.
    if (now_ - last_ms_ > P300_KW_ANSWER_TIMEOUT_MS) {
      const bool unheard = count_ == 0 || (count_ == 1 && kw_answer_[0] == ENQ);
      if (unheard && kw_retries_left_ != 0) {
        --kw_retries_left_;
        ++retries_;
        last_ms_ = now_;
        request_ms_ = now_;
        this->set_state_(State::KW_ENQ);
        return;
      }
      this->set_state_(State::RESET);
      this->fail_(OptolinkResult::TIMEOUT);
      return;
    }
    while (port_->available() && count_ < expected) {
      kw_answer_[count_++] = port_->read();
      last_ms_ = now_;
    }
    if (count_ < expected)
      return;
    // After a payload anything but 00 is the device's refusal.
    if (kw_has_payload_ && kw_answer_[0] != 0x00) {
      this->set_state_(State::RESET);
      this->fail_(OptolinkResult::DEVICE_ERROR);
      return;
    }
    this->set_state_(State::KW_CHAIN);
    this->deliver_(kw_answer_, expected, address_);
  }

  void kw_chain_() {
    if (port_->available()) {
      // The device's ENQ: the session idled out. It opens the next KW
      // telegram, or the handshake back to P300.
      if (port_->read() != ENQ) {
        this->set_state_(State::RESET);
        return;
      }
      if (busy_ && kw_) {
        this->kw_enq_read_();
      } else {
        last_ms_ = now_;
        this->set_state_(State::INIT);
        this->hold_(TimingPoint::ENQ_TO_SYNC);
      }
      return;
    }
    if (busy_ && kw_) {
      if (kw_type_ != PacketVS1Type.GFA_READ || now_ - last_ms_ >= KW_GFA_GAP_MS)
        this->kw_start_(false);
      return;
    }
    if (busy_ || now_ - last_ms_ > P300_KW_LINGER_MS)
      this->set_state_(State::RESET);
  }

  // --- the open frame ---------------------------------------------------------

  void frame_send_() {
    count_ =
        static_cast<uint8_t>(count_ + port_->write(&frame_bytes_[count_], static_cast<uint8_t>(frame_len_ - count_)));
    if (count_ == frame_len_) {
      last_ms_ = now_;
      // The answer gets the whole watchdog.
      request_ms_ = now_;
      this->set_state_(State::FRAME_RECEIVE);
    }
  }

  // Bytes past a fixed answer length stay on the line for RESET to drain.
  void frame_receive_() {
    while (port_->available() && (length_ == 0 || count_ < length_)) {
      const uint8_t b = port_->read();
      if (count_ < P300_OPEN_FRAME_MAX)
        frame_answer_[count_++] = b;
      last_ms_ = now_;
    }
    const bool complete = length_ != 0 && count_ == length_;
    const bool quiet = now_ - last_ms_ > P300_FRAME_QUIET_MS;
    if (!complete && !quiet)
      return;
    const uint8_t received = count_;
    this->set_state_(State::RESET);
    if (complete || (length_ == 0 && received != 0)) {
      this->deliver_(frame_answer_, received, 0);
      return;
    }
    this->fail_(OptolinkResult::TIMEOUT);
  }

  void deliver_(const uint8_t *data, uint8_t length, uint16_t address) {
    if (on_result_) {
      Result result{};
      result.status = OptolinkResult::OK;
      result.address = address;
      result.data = data;
      result.length = length;
      result.retries = retries_;
      on_result_(result);
    }
    busy_ = false;
  }

  void fail_(OptolinkResult status) {
    if (on_result_) {
      Result result{};
      result.status = status;
      result.address = address_;
      result.retries = retries_;
      if (status == OptolinkResult::DEVICE_ERROR && error_len_ != 0) {
        result.error_code = error_code_;
        result.error_code_length = error_len_;
      }
      on_result_(result);
    }
    busy_ = false;
  }

  Port *port_;
  State state_{State::UNDEFINED};
  uint32_t now_;
  uint32_t last_ms_;
  uint32_t request_ms_{0};
  // Bytes written or read within the state.
  uint8_t count_{0};
  // The timing points, and the state's held write: from when, how long.
  uint32_t timing_[TIMING_POINTS]{};
  uint32_t hold_from_ms_{0};
  uint32_t hold_ms_{0};
  bool waiting_{false};
  P300Parser parser_;
  uint16_t address_{0};
  uint8_t length_{0};
  bool busy_{false};
  Telegram telegram_{};
  // A KW telegram in flight, its frame (the 01 in front when it opens the
  // session) and its answer.
  bool kw_{false};
  uint8_t kw_type_{0};
  bool kw_has_payload_{false};
  uint8_t kw_payload_[P300_KW_MAX_PAYLOAD]{};
  uint8_t kw_frame_[5 + P300_KW_MAX_PAYLOAD]{};
  uint8_t kw_frame_len_{0};
  uint8_t kw_answer_[P300_KW_MAX_LENGTH]{};
  // Silence retries left for it, and those spent (Result::retries).
  uint8_t kw_retries_left_{0};
  uint8_t retries_{0};
  bool kw_dropped_{false};
  // The last device byte an EOT waits out.
  uint32_t heard_ms_{0};
  bool heard_{false};
  // An open frame in flight and its answer.
  bool frame_{false};
  uint8_t frame_bytes_[P300_OPEN_FRAME_MAX]{};
  uint8_t frame_len_{0};
  uint8_t frame_answer_[P300_OPEN_FRAME_MAX]{};
  uint8_t error_code_[P300_ERROR_CODE_MAX]{};
  uint8_t error_len_{0};
  ResultCallback on_result_;
};

}  // namespace esphome::vitohome::optolink

#endif  // VITOHOME_OPTOLINK_P300_H
