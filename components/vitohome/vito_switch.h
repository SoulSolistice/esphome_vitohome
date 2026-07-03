#pragma once
#include <vector>

#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "vito_entity.h"

namespace esphome::vitohome {

// Boolean writable datapoint exposed as an ESPHome switch: a two-state
// specialisation of VitoSelect for registers like NRx_Partybetrieb (0x2330,
// AUS/EIN) or the K-coding booleans, so Home Assistant gets a native toggle
// (switch.turn_on/off, voice assistants, binary automation conditions)
// instead of a two-option dropdown.
//
// on_value_/off_value_ are the raw wire values WRITTEN for on/off (default
// 1/0; configurable because e.g. K8A uses 175=aktiv / 176=inaktiv). For state
// decoding, any value in on_state_values_ publishes ON, off_value_ publishes
// OFF, and anything else is logged and left unpublished -- the same
// keep-last-and-warn policy VitoSelect applies to unmapped wire values.
//
// State always comes from the device (poll + read-back). Boot-time restore
// modes are deliberately unsupported: switch.py pins restore_mode to
// DISABLED, and this class never calls get_initial_state_with_restore_mode(),
// so a reboot can never write to the heater.
class VitoSwitch : public switch_::Switch, public Component, public VitoEntityBase {
 public:
  void set_on_value(uint32_t v) { this->on_value_ = v; }
  void set_off_value(uint32_t v) { this->off_value_ = v; }
  void add_on_state_value(uint32_t v) { this->on_state_values_.push_back(v); }
  void set_read_back(bool v) { this->read_back_ = v; }

  void dump_config() override;
  void handle_response(const ResponseView& response) override;
  void handle_write_response(const ResponseView& response) override;
  void handle_error(optolink::OptolinkResult error) override;
  const char* entity_kind() const override { return "switch"; }

 protected:
  void write_state(bool state) override;

  uint32_t on_value_{1};
  uint32_t off_value_{0};
  std::vector<uint32_t> on_state_values_;
  bool pending_state_{false};
};

}  // namespace esphome::vitohome
