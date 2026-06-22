#pragma once
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"
#include "vito_entity.h"

namespace esphome {
namespace vitohome {

class VitoSensor : public sensor::Sensor, public Component, public VitoEntityBase {
 public:
  // Component-side conversion (Stage 2): the raw little-endian payload is
  // extracted as int64/uint64, scaled in double, and only the final value is
  // narrowed to ESPHome's float32 state. This is what fixes the precision
  // loss of float32-pipeline decodes for 4-byte counters (see decode.h).
  void set_scale(double scale) { this->scale_ = scale; }
  void set_signed(bool s) { this->signed_ = s; }
  // Optional single-byte extraction: take payload[byte] (length-1 raw) and
  // apply scale/sign to that byte. Replaces the old ">> 8 & 0xFF" lambda
  // filters for the PR2 pump-speed unit.
  void set_extract_byte(int8_t byte) { this->extract_byte_ = byte; }

  void dump_config() override;
  void handle_response(const optolink::PacketVS2 &response) override;
  void handle_error(optolink::OptolinkResult error) override;
  const char *entity_kind() const override { return "sensor"; }

 protected:
  double scale_{1.0};
  bool signed_{false};
  int8_t extract_byte_{-1};
};

}  // namespace vitohome
}  // namespace esphome
