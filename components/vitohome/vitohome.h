#pragma once
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "optolink/optolink.h"
#include "protocol_adapter.h"
#include "vito_entity.h"
#include "vito_uart_interface.h"

namespace esphome {
namespace vitohome {

class VitoHomeComponent : public PollingComponent, public uart::UARTDevice {
 public:
  // ESPHomeUARTInterface stores `this` (as UARTDevice*); valid in the
  // member init list because base subobjects are already constructed.
  VitoHomeComponent() : iface_(this) {}

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void register_entity(VitoEntityBase *entity) {
    if (entity == nullptr) return;
    entity->set_vitohome_parent(this);
    this->entities_.push_back(entity);
  }

  // device_id text sensors don't poll the bus themselves — they subscribe to
  // the hub's one-shot identification result (see identification below).
  void register_device_id_sensor(text_sensor::TextSensor *ts) {
    if (ts != nullptr) this->device_id_sensors_.push_back(ts);
  }

  void set_identify_device(bool v) { this->identify_device_ = v; }

  // Queue a write for `entity` (payload already staged in the entity's write
  // buffer). Writes are dispatched with priority over reads; the newest
  // payload wins if the entity is re-controlled while still queued (the
  // entity owns the buffer). Returns false if the entity has no payload.
  bool request_write(VitoEntityBase *entity);

 protected:
  enum class OpType : uint8_t { NONE, READ, WRITE };

  // One-shot device identification, run right after the protocol handshake:
  //   step 0: read 0x00F8 length 4 (Identification + IdentificationExtension
  //           in one transaction — the layout Vitosoft itself matches on);
  //   fallback: four length-1 reads at 0xF8/0xF9/0xFA/0xFB (the length-1
  //           reads at F8/F9 are wire-confirmed on the reference unit),
  //           each one fail-soft.
  // Result is logged at INFO, shown in dump_config, and pushed to any
  // registered device_id text sensors.
  enum class IdentState : uint8_t {
    IDLE,
    READ4,
    READ_F8,
    READ_F9,
    READ_FA,
    READ_FB,
    DONE,
  };

  void validate_uart_();
  void dispatch_next_();
  void schedule_due_entities_();
  void on_response_(const ResponseView &response, const optolink::Datapoint &request);
  void on_error_(optolink::OptolinkResult error, const optolink::Datapoint &request);

  // identification
  void ident_start_();
  void ident_dispatch_(IdentState state);
  void ident_handle_response_(const ResponseView &response);
  void ident_handle_error_();
  void ident_finish_();
  std::string ident_string_() const;
  bool ident_in_flight_{false};

 private:
  ESPHomeUARTInterface iface_;
  // The protocol engine is build-time-selected and wrapped by ProtocolAdapter,
  // which presents a uniform, protocol-blind interface (ResponseView callbacks +
  // read/write/begin/loop) regardless of P300/KW/GWG. The adapter is the only
  // place that touches a concrete packet type.
  std::unique_ptr<ProtocolAdapter> vito_;

  std::vector<VitoEntityBase *> entities_;
  std::vector<text_sensor::TextSensor *> device_id_sensors_;
  std::deque<VitoEntityBase *> read_queue_;
  std::deque<VitoEntityBase *> write_queue_;
  VitoEntityBase *in_flight_{nullptr};
  OpType in_flight_op_{OpType::NONE};
  uint32_t in_flight_started_ms_{0};

  // identification state
  bool identify_device_{true};
  IdentState ident_state_{IdentState::IDLE};
  optolink::Datapoint ident_dp_{"ident", 0x00F8, 4, optolink::noconv};
  int ident_group_{-1}, ident_controller_{-1}, ident_hw_{-1}, ident_sw_{-1};

  // Failsafe: if a request is in flight for longer than this, log and
  // clear it. the optolink engine has its own internal timeout (via OptolinkResult
  // ::TIMEOUT) but if a callback is somehow lost the queue stalls.
  static constexpr uint32_t IN_FLIGHT_WATCHDOG_MS = 10000;

  // Start-up protocol check: the configured protocol must establish a link
  // (the adapter sees a first valid response) within this window, else the
  // component is marked failed. This catches a wrong protocol / wiring fault /
  // offline device instead of polling silently forever. The effective window is
  // max(this, 3x the hub update interval).
  static constexpr uint32_t PROTOCOL_VERIFY_MIN_MS = 90000;
  bool protocol_verify_pending_{false};
  uint32_t protocol_verify_deadline_ms_{0};
};

}  // namespace vitohome
}  // namespace esphome
