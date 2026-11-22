// Include guards, not #pragma once: see optolink_types.h.
#ifndef VITOHOME_OPTOLINK_ADAPTER_H
#define VITOHOME_OPTOLINK_ADAPTER_H

// The P300 and GWG engines behind the exchange's interface (optolink_exchange.h):
// submit() takes a Request, one Result reports how it ended, and the engine's
// getters become its fields. They stay until their links replace them, P300
// in stage 4 and GWG in stage 5 (design-optolink-layers.md, section 10).

#include <cstdint>
#include <functional>
#include <utility>

#include "optolink/optolink.h"
#include "optolink_types.h"

namespace esphome::vitohome::optolink {

template<class Port> class P300Adapter {
 public:
  using ResultCallback = std::function<void(const Result &)>;

  explicit P300Adapter(Port *port) : engine_(port) {
    engine_.onResponse([this](const uint8_t *data, uint8_t length, uint16_t address) {
      Result result{};
      result.status = OptolinkResult::OK;
      result.address = address;
      result.data = data;
      result.length = length;
      this->deliver_(result);
    });
    engine_.onError([this](OptolinkResult status, uint16_t address) {
      Result result{};
      result.status = status;
      result.address = address;
      if (status == OptolinkResult::DEVICE_ERROR) {
        result.error_code = engine_.errorPayload(result.error_code_length);
        if (result.error_code_length == 0)
          result.error_code = nullptr;
      }
      this->deliver_(result);
    });
  }
  P300Adapter(const P300Adapter &) = delete;
  P300Adapter &operator=(const P300Adapter &) = delete;

  void on_result(ResultCallback callback) { on_result_ = std::move(callback); }
  bool begin() { return engine_.begin(); }
  void loop() { engine_.loop(); }
  void end() { engine_.end(); }
  bool busy() const { return engine_.isBusy(); }
  bool wants_fast_loop() const { return false; }
  // No timing points on this engine.
  bool set_timing(TimingPoint, uint32_t &) { return false; }
  uint32_t timing(TimingPoint) const { return 0; }
  uint32_t timing_default(TimingPoint) const { return 0; }

  // A KW request takes the engine's detour to KW and back.
  bool submit(const Request &request) {
    const uint8_t *payload = request.has_payload ? request.payload : nullptr;
    switch (request.lane) {
      case Lane::P300:
        return engine_.request(request.code, request.address, request.length, payload);
      case Lane::KW:
        return engine_.kwTelegram(request.code, request.address, request.length, payload);
      case Lane::FRAME:
        return engine_.frame(request.payload, request.length, request.answer_length);
      case Lane::GWG:
      default:
        return false;
    }
  }

 private:
  void deliver_(const Result &result) {
    if (on_result_)
      on_result_(result);
  }

  VS2Engine engine_;
  ResultCallback on_result_;
};

template<class Port> class GwgAdapter {
 public:
  using ResultCallback = std::function<void(const Result &)>;
  using LateAnswer = GWGEngine::LateAnswer;

  explicit GwgAdapter(Port *port) : engine_(port) {
    // Fires for an answered read just before its response.
    engine_.onTiming([this](uint32_t, uint32_t send_to_response_ms, uint16_t) {
      answer_timed_ = true;
      send_to_answer_ms_ = send_to_response_ms;
    });
    engine_.onResponse([this](const uint8_t *data, uint8_t length, uint16_t address) {
      Result result = this->result_(OptolinkResult::OK, address);
      result.data = data;
      result.length = length;
      result.unserved_eeprom = engine_.lastReadHadUnservedEeprom();
      this->deliver_(result);
    });
    engine_.onError([this](OptolinkResult status, uint16_t address) {
      Result result = this->result_(status, address);
      result.never_sent = status == OptolinkResult::TIMEOUT && engine_.lastTimeoutWasUnsynced();
      this->deliver_(result);
    });
  }
  GwgAdapter(const GwgAdapter &) = delete;
  GwgAdapter &operator=(const GwgAdapter &) = delete;

  void on_result(ResultCallback callback) { on_result_ = std::move(callback); }
  bool begin() { return engine_.begin(); }
  void loop() { engine_.loop(); }
  void end() { engine_.end(); }
  bool busy() const { return engine_.isBusy(); }
  // While a request waits out the ENQ-to-send delay or the gap after the 01.
  bool wants_fast_loop() const { return engine_.isHoldingEnq(); }
  // GWG's own knobs (set_gwg_*) until its link has timing points.
  bool set_timing(TimingPoint, uint32_t &) { return false; }
  uint32_t timing(TimingPoint) const { return 0; }
  uint32_t timing_default(TimingPoint) const { return 0; }

  // A GWG request by its TYPE byte: an access mode's read or write keeps its
  // mode's handling, a two-byte EEPROM write is the pair as given.
  bool submit(const Request &request) {
    switch (request.lane) {
      case Lane::GWG:
        return engine_.telegram(request.code, request.address, request.length,
                                request.has_payload ? request.payload : nullptr);
      case Lane::FRAME:
        return engine_.frame(request.payload, request.length, request.answer_length);
      case Lane::P300:
      case Lane::KW:
      default:
        return false;
    }
  }

  // GWG's diagnostics (GWGEngine), until its timing points replace them.
  void on_burst_fallback(GWGEngine::OnBurstFallbackCallback callback) { engine_.onBurstFallback(std::move(callback)); }
  void on_late_answer(GWGEngine::OnLateAnswerCallback callback) { engine_.onLateAnswer(std::move(callback)); }
  void set_write_eot_before_payload(bool eot_first) { engine_.setWriteEotBeforePayload(eot_first); }
  void set_burst_omits_enq_ack(bool omit) { engine_.setBurstOmitsEnqAck(omit); }
  void set_response_timeout_ms(uint32_t ms) { engine_.setResponseTimeoutMs(ms); }
  void set_enq_send_delay_ms(uint32_t ms) { engine_.setEnqSendDelayMs(ms); }
  void set_enq_ack_gap_ms(uint32_t ms) { engine_.setEnqAckGapMs(ms); }

 private:
  Result result_(OptolinkResult status, uint16_t address) {
    Result result{};
    result.status = status;
    result.address = address;
    result.timing.enq_paced = engine_.lastSendWasEnqPaced();
    result.timing.enq_age_ms = engine_.lastEnqAgeMs();
    result.timing.enq_to_send_ms = engine_.lastEnqToSendMs();
    result.timing.ack_gap_ms = engine_.lastAckGapMs();
    result.timing.answer_timed = answer_timed_;
    result.timing.send_to_answer_ms = send_to_answer_ms_;
    return result;
  }
  void deliver_(const Result &result) {
    answer_timed_ = false;
    send_to_answer_ms_ = 0;
    if (on_result_)
      on_result_(result);
  }

  GWGEngine engine_;
  ResultCallback on_result_;
  bool answer_timed_{false};
  uint32_t send_to_answer_ms_{0};
};

}  // namespace esphome::vitohome::optolink

#endif  // VITOHOME_OPTOLINK_ADAPTER_H
