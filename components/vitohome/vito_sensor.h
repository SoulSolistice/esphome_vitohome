#pragma once
#include "esphome/core/defines.h"

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"
#include "vito_entity.h"

namespace esphome::vitohome {

class VitoSensor final : public sensor::Sensor, public Component, public VitoEntityBase {
 public:
  // Component-side conversion: the raw little-endian payload is
  // extracted as int64/uint64, scaled in double, and only the final value is
  // narrowed to ESPHome's float32 state. This is what fixes the precision
  // loss of float32-pipeline decodes for 4-byte counters (see decode.h).
  void set_scale(double scale) { this->scale_ = scale; }
  void set_signed(bool s) { this->signed_ = s; }
  // RotateBytes: assemble the raw integer big-endian (read_be) before scaling.
  void set_big_endian(bool b) { this->big_endian_ = b; }

  void dump_config() override;
  void handle_response(const ResponseView &response) override;
  void handle_error(optolink::OptolinkResult error) override;
  const char *entity_kind() const override { return "sensor"; }

 protected:
  // A failed read (protocol error, short or non-finite answer): publishes NAN
  // once UNKNOWN_AFTER_READ_FAILURES are reached (VitoEntityBase).
  void note_read_failure_();

  double scale_{1.0};
  bool signed_{false};
  bool big_endian_{false};
};

}  // namespace esphome::vitohome
#endif  // USE_SENSOR
