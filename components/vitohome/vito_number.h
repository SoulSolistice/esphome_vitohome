#pragma once
#include "esphome/core/defines.h"

#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#include "esphome/core/component.h"
#include "vito_entity.h"

namespace esphome::vitohome {

// Writable datapoint exposed as an ESPHome number. The write path encodes in
// double precision (decode.h::encode_scaled) and transmits via the optolink engine's
// raw-bytes write; the periodic read path keeps the entity in sync with
// changes made at the boiler panel. No flash preferences are used: the
// device itself is the source of truth, restored by the first poll.
class VitoNumber final : public number::Number, public Component, public VitoEntityBase {
 public:
  void set_scale(double scale) { this->scale_ = scale; }
  void set_signed(bool s) { this->signed_ = s; }
  void set_read_back(bool v) { this->read_back_ = v; }
  // Aligned block extraction on the state read -- identical semantics to
  // VitoSelect/VitoSwitch: the field is extract_len_ bytes (default 1) at
  // extract_byte_ inside the block read; the write datapoint carries the
  // field width (set by codegen), so control() encodes the field only.
  void set_extract_byte(int16_t byte) { this->extract_byte_ = byte; }
  void set_extract_len(uint8_t len) { this->extract_len_ = len; }

  void dump_config() override;
  void handle_response(const ResponseView &response) override;
  void handle_write_response(const ResponseView &response) override;
  void handle_error(optolink::OptolinkResult error) override;
  const char *entity_kind() const override { return "number"; }

 protected:
  void control(float value) override;

  // See VitoSensor: go unavailable only after a streak of failed READS. A
  // failed WRITE never blanks the state (the device value did not change);
  // the inherited handle_write_error() no-op covers that side.
  static constexpr uint8_t NAN_AFTER_CONSECUTIVE_READ_ERRORS = 3;

  double scale_{1.0};
  bool signed_{false};
  float pending_value_{0.0f};
  uint8_t consecutive_read_errors_{0};
  int16_t extract_byte_{-1};
  uint8_t extract_len_{1};  // field width to slice at extract_byte_ (1..4)
};

}  // namespace esphome::vitohome
#endif  // USE_NUMBER
