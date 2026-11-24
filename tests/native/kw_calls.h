#pragma once

// The calls the engine harnesses make, for both sides of the differential
// target: the reference engines (VS1Engine, VS2Engine) take read(),
// telegram(), kwTelegram(), frame() and the like and report through
// onResponse / onError; the live KW and P300 stacks (optolink_kw.h,
// optolink_p300.h) take a Request and report a Result, mapped back onto the
// same two callbacks so a harness's checks and diff_hooks.h events stay one
// set. The generic overloads also serve the GWG engine.

#include <cstdint>

#include "optolink_kw.h"
#include "optolink_p300.h"
#include "trace_sink.h"

namespace kw_calls {

namespace live = esphome::vitohome::optolink;

// VS1Engine's timing on the KW link: the ENQ answered and a chained telegram
// sent one pass later, no retry after silence. The live side of the
// differential target runs on it, so the comparison with the reference covers
// everything else; the new timing has its own proofs.
inline constexpr live::EnqLinkParams kVs1Timing{50, 500, 1000, 4000, false, 0, 0, 0};

// The KW link without its pauses: the proofs of chaining, GFA reads, writes
// and open frames count passes without moving the clock; the pauses have
// their own proof (proof_kw_timing.cpp).
inline constexpr live::EnqLinkParams kNoPause{50, 500, 1000, 4000, true, 1, 0, 0};

template<class E, class R, class F> void wire(E &e, R on_response, F on_error) {
  e.onResponse(on_response);
  e.onError(on_error);
}
template<class P, const live::EnqLinkParams &K, class R, class F>
void wire(live::KwExchange<P, K> &e, R on_response, F on_error) {
  e.set_sink(trace_sink::counting());
  e.on_result([on_response, on_error](const live::Result &r) {
    if (r.status == live::OptolinkResult::OK) {
      on_response(r.data, r.length, r.address);
    } else {
      on_error(r.status, r.address);
    }
  });
}

template<class E> bool busy(const E &e) { return e.isBusy(); }
template<class P, const live::EnqLinkParams &K> bool busy(const live::KwExchange<P, K> &e) { return e.busy(); }

template<class E> bool read(E &e, uint16_t address, uint8_t length) { return e.read(address, length); }
template<class P, const live::EnqLinkParams &K> bool read(live::KwExchange<P, K> &e, uint16_t address, uint8_t length) {
  return e.submit(live::make_request(live::Lane::KW, live::PacketVS1Type.READ, address, length, nullptr));
}

template<class E> bool write(E &e, uint16_t address, const uint8_t *data, uint8_t length) {
  return e.write(address, data, length);
}
template<class P, const live::EnqLinkParams &K>
bool write(live::KwExchange<P, K> &e, uint16_t address, const uint8_t *data, uint8_t length) {
  return data != nullptr &&
         e.submit(live::make_request(live::Lane::KW, live::PacketVS1Type.WRITE, address, length, data));
}

template<class E> bool gfa_read(E &e, uint16_t address, uint8_t length) { return e.gfaRead(address, length); }
template<class P, const live::EnqLinkParams &K>
bool gfa_read(live::KwExchange<P, K> &e, uint16_t address, uint8_t length) {
  return e.submit(live::make_request(live::Lane::KW, live::PacketVS1Type.GFA_READ, address, length, nullptr));
}

template<class E> bool telegram(E &e, uint8_t type, uint16_t address, uint8_t length, const uint8_t *data = nullptr) {
  return e.telegram(type, address, length, data);
}
template<class P, const live::EnqLinkParams &K>
bool telegram(live::KwExchange<P, K> &e, uint8_t type, uint16_t address, uint8_t length,
              const uint8_t *data = nullptr) {
  return e.submit(live::make_request(live::Lane::KW, type, address, length, data));
}

template<class E> bool frame(E &e, const uint8_t *bytes, uint8_t length, uint8_t answer_length) {
  return e.frame(bytes, length, answer_length);
}
template<class P, const live::EnqLinkParams &K>
bool frame(live::KwExchange<P, K> &e, const uint8_t *bytes, uint8_t length, uint8_t answer_length) {
  return e.submit(live::make_frame(bytes, length, answer_length));
}

// --- P300: the reference VS2Engine, the live P300Exchange ------------------

// on_error_code(code, length) runs before on_error for a DEVICE_ERROR that
// carries the error frame's payload.
template<class E, class R, class F, class C> void wire(E &e, R on_response, F on_error, C on_error_code) {
  e.onResponse(on_response);
  e.onError([&e, on_error, on_error_code](auto status, uint16_t address) {
    if (status == decltype(status)::DEVICE_ERROR) {
      uint8_t n = 0;
      const uint8_t *code = e.errorPayload(n);
      if (n != 0)
        on_error_code(code, n);
    }
    on_error(status, address);
  });
}
template<class P, uint8_t N, uint32_t Q, class R, class F, class C>
void wire(live::P300Exchange<P, N, Q> &e, R on_response, F on_error, C on_error_code) {
  e.set_sink(trace_sink::counting());
  e.on_result([on_response, on_error, on_error_code](const live::Result &r) {
    if (r.status == live::OptolinkResult::OK) {
      on_response(r.data, r.length, r.address);
      return;
    }
    if (r.status == live::OptolinkResult::DEVICE_ERROR && r.error_code_length != 0)
      on_error_code(r.error_code, r.error_code_length);
    on_error(r.status, r.address);
  });
}
template<class P, uint8_t N, uint32_t Q, class R, class F>
void wire(live::P300Exchange<P, N, Q> &e, R on_response, F on_error) {
  wire(e, on_response, on_error, [](const uint8_t *, uint8_t) {});
}

template<class P, uint8_t N, uint32_t Q> bool busy(const live::P300Exchange<P, N, Q> &e) { return e.busy(); }

template<class P, uint8_t N, uint32_t Q> bool read(live::P300Exchange<P, N, Q> &e, uint16_t address, uint8_t length) {
  return e.submit(
      live::make_request(live::Lane::P300, static_cast<uint8_t>(live::FunctionCode::READ), address, length, nullptr));
}

template<class P, uint8_t N, uint32_t Q>
bool write(live::P300Exchange<P, N, Q> &e, uint16_t address, const uint8_t *data, uint8_t length) {
  return data != nullptr &&
         e.submit(live::make_request(live::Lane::P300, static_cast<uint8_t>(live::FunctionCode::WRITE), address, length,
                                     data));
}

template<class E> bool rpc(E &e, uint16_t address, const uint8_t *parameter, uint8_t length) {
  return e.rpc(address, parameter, length);
}
template<class P, uint8_t N, uint32_t Q>
bool rpc(live::P300Exchange<P, N, Q> &e, uint16_t address, const uint8_t *parameter, uint8_t length) {
  if (length != 0 && parameter == nullptr)
    return false;
  return e.submit(live::make_request(live::Lane::P300, static_cast<uint8_t>(live::FunctionCode::RPC), address, length,
                                     length != 0 ? parameter : nullptr));
}

// A P300 telegram by its function code.
template<class E>
bool request(E &e, uint8_t function, uint16_t address, uint8_t length, const uint8_t *data = nullptr) {
  return e.request(function, address, length, data);
}
template<class P, uint8_t N, uint32_t Q>
bool request(live::P300Exchange<P, N, Q> &e, uint8_t function, uint16_t address, uint8_t length,
             const uint8_t *data = nullptr) {
  return e.submit(live::make_request(live::Lane::P300, function, address, length, data));
}

// A KW telegram over the P300 link's detour.
template<class E>
bool kw_telegram(E &e, uint8_t type, uint16_t address, uint8_t length, const uint8_t *data = nullptr) {
  return e.kwTelegram(type, address, length, data);
}
template<class P, uint8_t N, uint32_t Q>
bool kw_telegram(live::P300Exchange<P, N, Q> &e, uint8_t type, uint16_t address, uint8_t length,
                 const uint8_t *data = nullptr) {
  return e.submit(live::make_request(live::Lane::KW, type, address, length, data));
}

template<class P, uint8_t N, uint32_t Q>
bool gfa_read(live::P300Exchange<P, N, Q> &e, uint16_t address, uint8_t length) {
  return kw_telegram(e, live::PacketVS1Type.GFA_READ, address, length);
}

template<class P, uint8_t N, uint32_t Q>
bool frame(live::P300Exchange<P, N, Q> &e, const uint8_t *bytes, uint8_t length, uint8_t answer_length) {
  return e.submit(live::make_frame(bytes, length, answer_length));
}

}  // namespace kw_calls
