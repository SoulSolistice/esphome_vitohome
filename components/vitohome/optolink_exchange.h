// Include guards, not #pragma once: see optolink_types.h.
#ifndef VITOHOME_OPTOLINK_EXCHANGE_H
#define VITOHOME_OPTOLINK_EXCHANGE_H

// The exchange (design-optolink-layers.md, section 6): one request in flight,
// its codec and its link, its watchdog and its result.
//
//   submit()   the codec checks and encodes the request, the link takes the
//              telegram; false while busy or when the codec refuses it.
//   loop()     one pass of the link, then the watchdog. An answer goes
//              through the codec's verdict, then to the result callback.
//
// The request stays busy during its result callback, so a submit from inside
// it is refused.

#include <cstdint>
#include <functional>
#include <utility>

#include "optolink_clock.h"
#include "optolink_link_enq.h"
#include "optolink_types.h"

namespace esphome::vitohome::optolink {

template<class Codec, class Link> class Exchange {
 public:
  using ResultCallback = std::function<void(const Result &)>;

  template<class Port>
  Exchange(Port *port, const EnqLinkParams &params, uint32_t watchdog_ms)
      : link_(port, params), watchdog_ms_(watchdog_ms), now_(optolink_millis()) {}
  Exchange(const Exchange &) = delete;
  Exchange &operator=(const Exchange &) = delete;

  void on_result(ResultCallback callback) { on_result_ = std::move(callback); }

  bool begin() { return link_.begin(); }
  void end() {
    link_.end();
    link_.release();
  }
  bool busy() const { return link_.pending(); }

  bool submit(const Request &request) {
    if (link_.pending())
      return false;
    Telegram telegram;
    uint8_t expect = 0;
    bool collect = false;
    if (!Codec::prepare(request, telegram, expect, collect))
      return false;
    frame_ = request.lane == Lane::FRAME;
    address_ = frame_ ? 0 : request.address;
    // From the last pass's clock, as the watchdog compares against it.
    request_ms_ = now_;
    link_.start(telegram, expect, collect, frame_);
    return true;
  }

  void loop() {
    now_ = optolink_millis();
    switch (link_.loop(now_)) {
      case LinkEvent::SENT:
        // An open frame's watchdog runs from its last byte.
        if (frame_)
          request_ms_ = now_;
        break;
      case LinkEvent::ANSWER: {
        const OptolinkResult status = Codec::accept(link_.telegram(), link_.answer(), link_.answer_length());
        if (status == OptolinkResult::OK) {
          link_.chain();
          this->deliver_(status, link_.answer(), link_.answer_length());
        } else {
          link_.wait_enq();
          this->deliver_(status, nullptr, 0);
        }
        break;
      }
      case LinkEvent::COLLECTED:
        this->deliver_(OptolinkResult::OK, link_.answer(), link_.answer_length());
        break;
      case LinkEvent::TIMEOUT:
        this->deliver_(OptolinkResult::TIMEOUT, nullptr, 0);
        break;
      case LinkEvent::NONE:
        break;
    }
    if (link_.pending() && now_ - request_ms_ > watchdog_ms_) {
      link_.abort();
      this->deliver_(OptolinkResult::TIMEOUT, nullptr, 0);
    }
  }

 private:
  void deliver_(OptolinkResult status, const uint8_t *data, uint8_t length) {
    if (on_result_)
      on_result_(Result{status, address_, data, length});
    link_.release();
  }

  Link link_;
  uint32_t watchdog_ms_;
  uint32_t now_;
  uint32_t request_ms_{0};
  uint16_t address_{0};
  bool frame_{false};
  ResultCallback on_result_;
};

}  // namespace esphome::vitohome::optolink

#endif  // VITOHOME_OPTOLINK_EXCHANGE_H
