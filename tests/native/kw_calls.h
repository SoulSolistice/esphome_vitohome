#pragma once

// The KW calls the engine harnesses make, for both sides of the differential
// target: the reference VS1Engine takes read(), telegram() and frame() and
// reports through onResponse / onError; the live KW stack (optolink_kw.h)
// takes a Request and reports a Result, mapped back onto the same two
// callbacks so a harness's checks and diff_hooks.h events stay one set. The
// generic overloads also serve the P300 and GWG engines.

#include <cstdint>

#include "optolink_kw.h"

namespace kw_calls {

namespace live = esphome::vitohome::optolink;

template<class E, class R, class F> void wire(E &e, R on_response, F on_error) {
  e.onResponse(on_response);
  e.onError(on_error);
}
template<class P, class R, class F> void wire(live::KwExchange<P> &e, R on_response, F on_error) {
  e.on_result([on_response, on_error](const live::Result &r) {
    if (r.status == live::OptolinkResult::OK) {
      on_response(r.data, r.length, r.address);
    } else {
      on_error(r.status, r.address);
    }
  });
}

template<class E> bool busy(const E &e) { return e.isBusy(); }
template<class P> bool busy(const live::KwExchange<P> &e) { return e.busy(); }

template<class E> bool read(E &e, uint16_t address, uint8_t length) { return e.read(address, length); }
template<class P> bool read(live::KwExchange<P> &e, uint16_t address, uint8_t length) {
  return e.submit(live::make_request(live::Lane::KW, live::PacketVS1Type.READ, address, length, nullptr));
}

template<class E> bool write(E &e, uint16_t address, const uint8_t *data, uint8_t length) {
  return e.write(address, data, length);
}
template<class P> bool write(live::KwExchange<P> &e, uint16_t address, const uint8_t *data, uint8_t length) {
  return data != nullptr &&
         e.submit(live::make_request(live::Lane::KW, live::PacketVS1Type.WRITE, address, length, data));
}

template<class E> bool gfa_read(E &e, uint16_t address, uint8_t length) { return e.gfaRead(address, length); }
template<class P> bool gfa_read(live::KwExchange<P> &e, uint16_t address, uint8_t length) {
  return e.submit(live::make_request(live::Lane::KW, live::PacketVS1Type.GFA_READ, address, length, nullptr));
}

template<class E> bool telegram(E &e, uint8_t type, uint16_t address, uint8_t length, const uint8_t *data = nullptr) {
  return e.telegram(type, address, length, data);
}
template<class P>
bool telegram(live::KwExchange<P> &e, uint8_t type, uint16_t address, uint8_t length, const uint8_t *data = nullptr) {
  return e.submit(live::make_request(live::Lane::KW, type, address, length, data));
}

template<class E> bool frame(E &e, const uint8_t *bytes, uint8_t length, uint8_t answer_length) {
  return e.frame(bytes, length, answer_length);
}
template<class P> bool frame(live::KwExchange<P> &e, const uint8_t *bytes, uint8_t length, uint8_t answer_length) {
  return e.submit(live::make_frame(bytes, length, answer_length));
}

}  // namespace kw_calls
