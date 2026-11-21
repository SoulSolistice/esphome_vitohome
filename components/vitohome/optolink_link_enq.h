// Include guards, not #pragma once: see optolink_types.h.
#ifndef VITOHOME_OPTOLINK_LINK_ENQ_H
#define VITOHOME_OPTOLINK_LINK_ENQ_H

// The ENQ-paced link (design-optolink-layers.md, section 5.1): who may speak
// when on a line where the device offers each turn with an ENQ. It moves the
// exchange's telegram onto the wire and the answer off it, and knows nothing
// of what the bytes mean.
//
//   WAIT_ENQ   one byte read per pass; an ENQ opens ENQ_HELD. Without one for
//              eot_interval_ms, an EOT (it ends a P300 session the device may
//              still be in).
//   ENQ_HELD   for enq_hold_ms: with a telegram pending, 01 and the telegram
//              (an open frame without the 01); with same_pass in the pass
//              that read the ENQ, else from the next one on.
//   SEND       the telegram; a partial write resumes on the next pass.
//   RECEIVE    the answer: `expect` bytes in one go, or while collecting,
//              whatever arrives until the line is quiet; the deadline is
//              checked before the drain and restarts on every byte. Silence
//              until the deadline, or a lone ENQ, sends a telegram (not an
//              open frame) again on the device's next ENQ, up to
//              silence_retries times.
//   CHAINED    after an answer, for chain_window_ms: the next telegram goes
//              out without an ENQ or 01, no earlier than its chain_gap_ms
//              after the answer; with same_pass from start() (poke()), else
//              on the pass after it was taken. Any byte on the line ends the
//              chain; so does an open frame.
//
// The exchange decides what an answer is worth and moves the link on: chain()
// after a good one, wait_enq() after a refused one, abort() on its watchdog.

#include <cstddef>
#include <cstdint>

#include "optolink_clock.h"
#include "optolink_types.h"

namespace esphome::vitohome::optolink {

struct EnqLinkParams {
  // How long after the ENQ it may still be answered.
  uint32_t enq_hold_ms;
  // How long after an answer a telegram may be chained onto it.
  uint32_t chain_window_ms;
  // How long the line may stay silent while an answer is due.
  uint32_t answer_timeout_ms;
  // The EOT while no ENQ comes; 0: none.
  uint32_t eot_interval_ms;
  // Answer an ENQ, and chain a telegram, in the pass that allows it; false:
  // one pass later.
  bool same_pass;
  // How often a telegram met by silence goes out again, on the next ENQ.
  uint8_t silence_retries;
};

// RETRY: silence until the deadline; the telegram waits for the next ENQ.
enum class LinkEvent : uint8_t { NONE, SENT, ANSWER, COLLECTED, TIMEOUT, RETRY };

template<class Port> class EnqLink {
 public:
  // The longest answer the link holds.
  static constexpr std::size_t ANSWER_MAX = MAX_PAYLOAD;

  EnqLink(Port *port, const EnqLinkParams &params) : port_(port), params_(params), last_ms_(optolink_millis()) {}

  bool begin() {
    if (!port_->begin())
      return false;
    while (port_->available())
      port_->read();
    last_ms_ = optolink_millis();
    state_ = State::WAIT_ENQ;
    return true;
  }

  void end() {
    port_->end();
    state_ = State::UNDEFINED;
  }

  // The telegram goes out when the line allows; `expect` answer bytes, or
  // with `collect` until the line is quiet.
  void start(const Telegram &telegram, uint8_t expect, bool collect, bool frame) {
    telegram_ = telegram;
    expect_ = expect;
    collect_ = collect;
    frame_ = frame;
    pending_ = true;
    retries_left_ = params_.silence_retries;
  }
  // With same_pass, what start() allows at once: a telegram onto a held ENQ
  // or onto a chain.
  LinkEvent poke(uint32_t now) {
    if (!params_.same_pass)
      return LinkEvent::NONE;
    if (state_ == State::ENQ_HELD)
      return this->enq_held_(now);
    if (state_ == State::CHAINED)
      return this->chained_(now);
    return LinkEvent::NONE;
  }
  bool pending() const { return pending_; }
  void release() { pending_ = false; }

  // A good answer: chain the next telegram, unless it was an open frame's.
  void chain() { state_ = frame_ ? State::WAIT_ENQ : State::CHAINED; }
  // A refused answer: the next telegram waits for an ENQ.
  void wait_enq() { state_ = State::WAIT_ENQ; }
  // The exchange's watchdog.
  void abort() {
    count_ = 0;
    state_ = State::WAIT_ENQ;
  }

  const uint8_t *answer() const { return answer_; }
  uint8_t answer_length() const { return answer_length_; }
  const Telegram &telegram() const { return telegram_; }

  // One pass.
  LinkEvent loop(uint32_t now) {
    switch (state_) {
      case State::WAIT_ENQ:
        return this->wait_for_enq_(now);
      case State::ENQ_HELD:
        return this->enq_held_(now);
      case State::CHAINED:
        return this->chained_(now);
      case State::SEND:
        return this->send_(now);
      case State::RECEIVE:
        return this->receive_(now);
      case State::UNDEFINED:
        break;
    }
    return LinkEvent::NONE;
  }

 private:
  enum class State : uint8_t { UNDEFINED, WAIT_ENQ, ENQ_HELD, CHAINED, SEND, RECEIVE };

  static constexpr uint8_t ENQ = 0x05;
  static constexpr uint8_t ENQ_ACK = 0x01;
  static constexpr uint8_t EOT = 0x04;

  LinkEvent wait_for_enq_(uint32_t now) {
    if (port_->available()) {
      if (port_->read() == ENQ) {
        last_ms_ = now;
        state_ = State::ENQ_HELD;
        if (params_.same_pass)
          return this->enq_held_(now);
      }
    } else if (params_.eot_interval_ms != 0 && now - last_ms_ > params_.eot_interval_ms) {
      last_ms_ = now;
      port_->write(&EOT, 1);
    }
    return LinkEvent::NONE;
  }

  LinkEvent enq_held_(uint32_t now) {
    if (now - last_ms_ >= params_.enq_hold_ms) {
      state_ = State::WAIT_ENQ;
      return LinkEvent::NONE;
    }
    if (pending_ && (frame_ || port_->write(&ENQ_ACK, 1) == 1)) {
      state_ = State::SEND;
      return this->send_(now);
    }
    return LinkEvent::NONE;
  }

  LinkEvent chained_(uint32_t now) {
    // A byte since the answer is the device's next ENQ: never chain across it.
    if (port_->available() || (pending_ && frame_)) {
      state_ = State::WAIT_ENQ;
      return LinkEvent::NONE;
    }
    if (now - last_ms_ >= params_.chain_window_ms) {
      state_ = State::WAIT_ENQ;
      return LinkEvent::NONE;
    }
    if (!pending_ || now - last_ms_ < telegram_.chain_gap_ms)
      return LinkEvent::NONE;
    state_ = State::SEND;
    return params_.same_pass ? this->send_(now) : LinkEvent::NONE;
  }

  LinkEvent send_(uint32_t now) {
    count_ = static_cast<uint8_t>(count_ + port_->write(&telegram_.bytes[count_], telegram_.length - count_));
    if (count_ != telegram_.length)
      return LinkEvent::NONE;
    count_ = 0;
    last_ms_ = now;
    state_ = State::RECEIVE;
    return LinkEvent::SENT;
  }

  LinkEvent receive_(uint32_t now) {
    if (now - last_ms_ > params_.answer_timeout_ms) {
      const uint8_t received = count_;
      count_ = 0;
      state_ = State::WAIT_ENQ;
      if (collect_ && received != 0) {
        answer_length_ = received < ANSWER_MAX ? received : static_cast<uint8_t>(ANSWER_MAX);
        return LinkEvent::COLLECTED;
      }
      // Silence, or nothing but the device's next ENQ: the telegram went
      // unheard.
      const bool unheard = received == 0 || (received == 1 && answer_[0] == ENQ);
      if (unheard && !frame_ && retries_left_ != 0) {
        --retries_left_;
        return LinkEvent::RETRY;
      }
      return LinkEvent::TIMEOUT;
    }
    while (port_->available()) {
      const uint8_t b = port_->read();
      if (count_ < ANSWER_MAX)
        answer_[count_] = b;
      if (count_ < 0xFF)
        ++count_;
      last_ms_ = now;
    }
    // Exactly `expect`: an answer longer than that runs into the deadline.
    if (collect_ || count_ != expect_)
      return LinkEvent::NONE;
    answer_length_ = expect_;
    count_ = 0;
    return LinkEvent::ANSWER;
  }

  Port *port_;
  EnqLinkParams params_;
  State state_{State::UNDEFINED};
  uint32_t last_ms_;
  Telegram telegram_{};
  uint8_t expect_{0};
  bool collect_{false};
  bool frame_{false};
  bool pending_{false};
  uint8_t retries_left_{0};
  // Bytes written of the telegram while sending, of the answer while
  // receiving.
  uint8_t count_{0};
  uint8_t answer_[ANSWER_MAX]{};
  uint8_t answer_length_{0};
};

}  // namespace esphome::vitohome::optolink

#endif  // VITOHOME_OPTOLINK_LINK_ENQ_H
