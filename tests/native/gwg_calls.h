#pragma once

// The calls the GWG harnesses make, for both sides of the differential
// target: the reference GWGEngine takes read(), write(), telegram() and
// frame() and reports through its five callbacks; the live GwgExchange
// (optolink_gwg.h) takes a Request and reports a Result, mapped back onto the
// same callbacks, so a harness's checks and diff_hooks.h events stay one set.

#include <cstdint>

#include "optolink_gwg.h"

namespace gwg_calls {

namespace live = esphome::vitohome::optolink;

template<class P> using Exchange = live::GwgExchange<P>;

// How the last result's telegram went out: the live side's answer to the
// reference engine's lastSendWasEnqPaced() and lastEnqToSendMs().
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

// The timing report comes before its answer, as GWGEngine::onTiming does.
template<class E, class R, class F, class T> void wire(E &e, R on_response, F on_error, T on_timing) {
  e.onResponse(on_response);
  e.onError(on_error);
  e.onTiming(on_timing);
}
template<class P, class R, class F, class T> void wire(Exchange<P> &e, R on_response, F on_error, T on_timing) {
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
  e.set_enq_send_delay_ms(delay_ms);
  e.set_response_timeout_ms(deadline_ms);
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

// As GWGEngine::write(): a mode without a write TYPE byte, and an EEPROM write
// of other than one cell, are refused. The two-byte EEPROM pair is
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
