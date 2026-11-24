#pragma once

// The calls the GWG harnesses and proofs make: read(), write(), telegram()
// and frame(), each a Request to GwgExchange (optolink_gwg.h), its Result
// reported through five callbacks, so a harness's checks and diff_hooks.h
// events stay one set.

#include <cstdint>
#include <functional>
#include <utility>

#include "optolink_gwg.h"
#include "trace_sink.h"

namespace gwg_calls {

namespace live = esphome::vitohome::optolink;

template<class P> using Exchange = live::GwgExchange<P>;

// The GWG exchange behind those calls, for the proofs written as calls. The
// last...() getters read
// the last Result; the settings map onto the timing points, the answer
// deadline and the variant switches.
template<class Port> class Engine {
 public:
  using LateAnswer = live::GwgLateAnswer;
  using OnResponse = std::function<void(const uint8_t *, uint8_t, uint16_t)>;
  using OnError = std::function<void(live::OptolinkResult, uint16_t)>;
  using OnTiming = std::function<void(uint32_t, uint32_t, uint16_t)>;

  explicit Engine(Port *port) : x_(port) {
    x_.set_sink(trace_sink::counting());
    x_.on_result([this](const live::Result &r) { this->deliver_(r); });
  }

  void onResponse(OnResponse callback) { on_response_ = std::move(callback); }
  void onError(OnError callback) { on_error_ = std::move(callback); }
  void onTiming(OnTiming callback) { on_timing_ = std::move(callback); }
  void onBurstFallback(std::function<void(uint8_t)> callback) { x_.on_burst_fallback(std::move(callback)); }
  void onLateAnswer(std::function<void(const LateAnswer &)> callback) { x_.on_late_answer(std::move(callback)); }

  bool begin() { return x_.begin(); }
  void loop() { x_.loop(); }
  void end() { x_.end(); }
  bool isBusy() const { return x_.busy(); }
  bool isHoldingEnq() const { return x_.wants_fast_loop(); }

  bool read(uint16_t address, uint8_t length, live::GWGAccessMode mode = live::GWGAccessMode::PHYSICAL) {
    return this->submit_(live::make_request(live::Lane::GWG, live::gwgReadTypeByte(mode), address, length, nullptr));
  }
  // A mode without a write TYPE byte, or an EEPROM write of other than one
  // cell, is refused.
  bool write(uint16_t address, const uint8_t *data, uint8_t length,
             live::GWGAccessMode mode = live::GWGAccessMode::PHYSICAL) {
    if (data == nullptr || !live::gwgModeIsWritable(mode) || (mode == live::GWGAccessMode::EEPROM && length != 1))
      return false;
    return this->submit_(live::make_request(live::Lane::GWG, live::gwgWriteTypeByte(mode), address, length, data));
  }
  bool telegram(uint8_t type, uint16_t address, uint8_t length, const uint8_t *data = nullptr) {
    return this->submit_(live::make_request(live::Lane::GWG, type, address, length, data));
  }
  bool frame(const uint8_t *bytes, uint8_t length, uint8_t answer_length) {
    return this->submit_(live::make_frame(bytes, length, answer_length));
  }

  bool lastSendWasEnqPaced() const { return last_.timing.enq_paced; }
  uint32_t lastEnqToSendMs() const { return last_.timing.enq_to_send_ms; }
  uint32_t lastEnqAgeMs() const { return last_.timing.enq_age_ms; }
  uint32_t lastAckGapMs() const { return last_.timing.ack_gap_ms; }
  bool lastTimeoutWasUnsynced() const { return last_.never_sent; }
  bool lastReadHadUnservedEeprom() const { return last_.unserved_eeprom; }

  void setWriteEotBeforePayload(bool eot_first) { x_.set_write_eot_before_payload(eot_first); }
  bool writeEotBeforePayload() const { return x_.write_eot_before_payload(); }
  void setBurstOmitsEnqAck(bool omit) { x_.set_burst_omits_enq_ack(omit); }
  bool burstOmitsEnqAck() const { return x_.burst_omits_enq_ack(); }
  void setResponseTimeoutMs(uint32_t ms) { x_.set_window(live::LinkWindow::ANSWER_DEADLINE, ms); }
  uint32_t responseTimeoutMs() const { return x_.window(live::LinkWindow::ANSWER_DEADLINE); }
  void setEnqSendDelayMs(uint32_t ms) { x_.set_timing(live::TimingPoint::ENQ_TO_SEND, ms); }
  uint32_t enqSendDelayMs() const { return x_.timing(live::TimingPoint::ENQ_TO_SEND); }
  void setEnqAckGapMs(uint32_t ms) { x_.set_timing(live::TimingPoint::ACK_TO_TELEGRAM, ms); }
  uint32_t enqAckGapMs() const { return x_.timing(live::TimingPoint::ACK_TO_TELEGRAM); }

  Exchange<Port> &exchange() { return x_; }

 private:
  // A new request clears the two flags.
  bool submit_(const live::Request &request) {
    if (!x_.submit(request))
      return false;
    last_.never_sent = false;
    last_.unserved_eeprom = false;
    return true;
  }
  void deliver_(const live::Result &r) {
    last_ = r;
    if (r.status != live::OptolinkResult::OK) {
      if (on_error_)
        on_error_(r.status, r.address);
      return;
    }
    if (r.timing.answer_timed && on_timing_)
      on_timing_(r.timing.enq_age_ms, r.timing.send_to_answer_ms, r.address);
    if (on_response_)
      on_response_(r.data, r.length, r.address);
  }

  Exchange<Port> x_;
  live::Result last_{};
  OnResponse on_response_;
  OnError on_error_;
  OnTiming on_timing_;
};

// How the last result's telegram went out.
struct Sent {
  bool enq_paced;
  uint32_t enq_to_send_ms;
};
inline Sent &last_sent() {
  static Sent sent{};
  return sent;
}

template<class E, class R, class F> void wire(E &e, R on_response, F on_error) {
  e.onResponse(on_response);
  e.onError(on_error);
}
template<class P, class R, class F> void wire(Exchange<P> &e, R on_response, F on_error) {
  wire(e, on_response, on_error, [](uint32_t, uint32_t, uint16_t) {});
}

// The timing report comes before its answer.
template<class E, class R, class F, class T> void wire(E &e, R on_response, F on_error, T on_timing) {
  e.onResponse(on_response);
  e.onError(on_error);
  e.onTiming(on_timing);
}
template<class P, class R, class F, class T> void wire(Exchange<P> &e, R on_response, F on_error, T on_timing) {
  e.set_sink(trace_sink::counting());
  e.on_result([on_response, on_error, on_timing](const live::Result &r) {
    last_sent() = Sent{r.timing.enq_paced, r.timing.enq_to_send_ms};
    if (r.status != live::OptolinkResult::OK) {
      on_error(r.status, r.address);
      return;
    }
    if (r.timing.answer_timed)
      on_timing(r.timing.enq_age_ms, r.timing.send_to_answer_ms, r.address);
    on_response(r.data, r.length, r.address);
  });
}

template<class E, class B, class L> void wire_diagnostics(E &e, B on_fallback, L on_late) {
  e.onBurstFallback(on_fallback);
  e.onLateAnswer(on_late);
}
template<class P, class B, class L> void wire_diagnostics(Exchange<P> &e, B on_fallback, L on_late) {
  e.on_burst_fallback(on_fallback);
  e.on_late_answer(on_late);
}

template<class E> void configure(E &e, bool burst_omits_ack, bool eot_first, uint32_t delay_ms, uint32_t deadline_ms) {
  e.setBurstOmitsEnqAck(burst_omits_ack);
  e.setWriteEotBeforePayload(eot_first);
  e.setEnqSendDelayMs(delay_ms);
  e.setResponseTimeoutMs(deadline_ms);
}
template<class P>
void configure(Exchange<P> &e, bool burst_omits_ack, bool eot_first, uint32_t delay_ms, uint32_t deadline_ms) {
  e.set_burst_omits_enq_ack(burst_omits_ack);
  e.set_write_eot_before_payload(eot_first);
  e.set_timing(live::TimingPoint::ENQ_TO_SEND, delay_ms);
  e.set_window(live::LinkWindow::ANSWER_DEADLINE, deadline_ms);
}

template<class E> bool busy(const E &e) { return e.isBusy(); }
template<class P> bool busy(const Exchange<P> &e) { return e.busy(); }

template<class E> bool holding(const E &e) { return e.isHoldingEnq(); }
template<class P> bool holding(const Exchange<P> &e) { return e.wants_fast_loop(); }

template<class E> Sent sent(const E &e) { return Sent{e.lastSendWasEnqPaced(), e.lastEnqToSendMs()}; }
template<class P> Sent sent(const Exchange<P> &) { return last_sent(); }

template<class E, class M> bool read(E &e, uint16_t address, uint8_t length, M mode) {
  return e.read(address, length, mode);
}
template<class P>
bool read(Exchange<P> &e, uint16_t address, uint8_t length, live::GWGAccessMode mode = live::GWGAccessMode::PHYSICAL) {
  return e.submit(live::make_request(live::Lane::GWG, live::gwgReadTypeByte(mode), address, length, nullptr));
}
template<class E> bool read(E &e, uint16_t address, uint8_t length) { return e.read(address, length); }

// A mode without a write TYPE byte, and an EEPROM write of other than one
// cell, are refused. The two-byte EEPROM pair is
// telegram()'s.
template<class E, class M> bool write(E &e, uint16_t address, const uint8_t *data, uint8_t length, M mode) {
  return e.write(address, data, length, mode);
}
template<class P>
bool write(Exchange<P> &e, uint16_t address, const uint8_t *data, uint8_t length,
           live::GWGAccessMode mode = live::GWGAccessMode::PHYSICAL) {
  if (data == nullptr || !live::gwgModeIsWritable(mode) || (mode == live::GWGAccessMode::EEPROM && length != 1))
    return false;
  return e.submit(live::make_request(live::Lane::GWG, live::gwgWriteTypeByte(mode), address, length, data));
}
template<class E> bool write(E &e, uint16_t address, const uint8_t *data, uint8_t length) {
  return e.write(address, data, length);
}

template<class E> bool telegram(E &e, uint8_t type, uint16_t address, uint8_t length, const uint8_t *data = nullptr) {
  return e.telegram(type, address, length, data);
}
template<class P>
bool telegram(Exchange<P> &e, uint8_t type, uint16_t address, uint8_t length, const uint8_t *data = nullptr) {
  return e.submit(live::make_request(live::Lane::GWG, type, address, length, data));
}

template<class E> bool frame(E &e, const uint8_t *bytes, uint8_t length, uint8_t answer_length) {
  return e.frame(bytes, length, answer_length);
}
template<class P> bool frame(Exchange<P> &e, const uint8_t *bytes, uint8_t length, uint8_t answer_length) {
  return e.submit(live::make_frame(bytes, length, answer_length));
}

}  // namespace gwg_calls
