#pragma once
#include "esphome/components/number/number.h"
#include "esphome/core/component.h"
#include "vito_entity.h"

namespace esphome {
namespace vitohome {

// Writable datapoint exposed as an ESPHome number. The write path encodes in
// double precision (decode.h::encode_scaled) and transmits via the optolink engine's
// raw-bytes write; the periodic read path keeps the entity in sync with
// changes made at the boiler panel. No flash preferences are used: the
// device itself is the source of truth, restored by the first poll.
class VitoNumber : public number::Number, public Component, public VitoEntityBase {
 public:
  void set_scale(double scale) { this->scale_ = scale; }
  void set_signed(bool s) { this->signed_ = s; }
  void set_read_back(bool v) { this->read_back_ = v; }

  void dump_config() override;
  void handle_response(const ResponseView& response) override;
  void handle_write_response(const ResponseView& response) override;
  void handle_error(optolink::OptolinkResult error) override;
  const char* entity_kind() const override { return "number"; }

 protected:
  void control(float value) override;

  double scale_{1.0};
  bool signed_{false};
  float pending_value_{0.0f};
};

}  // namespace vitohome
}  // namespace esphome
