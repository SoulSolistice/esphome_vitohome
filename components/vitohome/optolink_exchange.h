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
      : link_(port, params), defaults_(params), watchdog_ms_(watchdog_ms), now_(optolink_millis()) {}
  Exchange(const Exchange &) = delete;
  Exchange &operator=(const Exchange &) = delete;

  void on_result(ResultCallback callback) { on_result_ = std::move(callback); }
  // The trace for the recorder (LinkSink); none by default.
  void set_sink(const LinkSink &sink) { link_.set_sink(sink); }
  static const char *state_name(uint8_t state) { return Link::state_name(state); }

  // A restart takes the clock afresh: the exchange may not have been looped
  // for a while (a timed frame held the port), and a request submitted before
  // the next pass would take its watchdog from the old time.
  bool begin() {
    now_ = optolink_millis();
    return link_.begin();
  }
  void end() {
    link_.end();
    link_.release();
  }
  bool busy() const { return link_.pending(); }
  // Whether a write waits on the clock, so the main loop must not idle and
  // stretch the pause.
  bool wants_fast_loop() const { return link_.write_waits(); }

  // A timing point, `ms` clamped to its bound; false for a point the link
  // lacks.
  bool set_timing(TimingPoint point, uint32_t &ms) { return link_.set_timing(point, ms); }
  uint32_t timing(TimingPoint point) const { return link_.timing(point); }
  uint32_t timing_default(TimingPoint point) const {
    switch (point) {
      case TimingPoint::ENQ_TO_SEND:
        return defaults_.enq_to_send_ms;
      case TimingPoint::ANSWER_TO_NEXT:
        return defaults_.answer_to_next_ms;
      default:
        return 0;
    }
  }
  // No settable window yet.
  bool set_window(LinkWindow, uint32_t &) { return false; }
  uint32_t window(LinkWindow) const { return 0; }
  uint32_t window_default(LinkWindow) const { return 0; }

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
    retries_ = 0;
    link_.start(telegram, expect, collect, frame_);
    // On the clock as it is now: submit() may come long after the last pass.
    this->sent_(link_.poke(optolink_millis()));
    return true;
  }

  void loop() {
    now_ = optolink_millis();
    switch (const LinkEvent event = link_.loop(now_)) {
      case LinkEvent::SENT:
        this->sent_(event);
        break;
      case LinkEvent::RETRY:
        // The watchdog runs afresh for the telegram's second time out.
        request_ms_ = now_;
        ++retries_;
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
      link_.sink().fact(LinkFact::WATCHDOG);
      link_.abort();
      this->deliver_(OptolinkResult::TIMEOUT, nullptr, 0);
    }
  }

 private:
  // An open frame's watchdog runs from its last byte.
  void sent_(LinkEvent event) {
    if (event == LinkEvent::SENT && frame_)
      request_ms_ = now_;
  }
  void deliver_(OptolinkResult status, const uint8_t *data, uint8_t length) {
    if (on_result_) {
      Result result{};
      result.status = status;
      result.address = address_;
      result.data = data;
      result.length = length;
      result.retries = retries_;
      on_result_(result);
    }
    link_.release();
  }

  Link link_;
  const EnqLinkParams defaults_;
  uint32_t watchdog_ms_;
  uint32_t now_;
  uint32_t request_ms_{0};
  uint16_t address_{0};
  bool frame_{false};
  uint8_t retries_{0};
  ResultCallback on_result_;
};

}  // namespace esphome::vitohome::optolink

#endif  // VITOHOME_OPTOLINK_EXCHANGE_H
