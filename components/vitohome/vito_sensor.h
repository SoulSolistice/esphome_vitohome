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
  // Optional field extraction from a WIDER block read. The datapoint's length
  // is the number of bytes fetched from the wire (the whole firmware block, so
  // P300 gets an aligned read at the block base); this selects the FIELD
  // within that payload -- `byte` is the start offset (0..block-1) and
  // `set_extract_len` the field width (1..4 bytes, default 1) -- to scale/sign
  // as the value. So a 2-byte room-setpoint or a 4-byte solar-yield counter
  // sitting deep inside a 22- or 32-byte block is reachable with one aligned
  // read. -1 disables extraction (the whole length is decoded as one integer).
  void set_extract_byte(int16_t byte) { this->extract_byte_ = byte; }
  void set_extract_len(uint8_t len) { this->extract_len_ = len; }

  void dump_config() override;
  void handle_response(const ResponseView &response) override;
  void handle_error(optolink::OptolinkResult error) override;
  const char *entity_kind() const override { return "sensor"; }

 protected:
  // Advance the consecutive-read-error streak and blank the entity (publish
  // NAN) once it crosses the threshold. Shared by handle_error (protocol error)
  // and handle_response (a decode failure -- short or non-finite payload), so a
  // persistently bad read eventually goes unavailable instead of pinning the
  // last good value.
  void note_read_failure_();

  // A single transient bus error (CRC glitch, one timeout) used to publish NAN
  // immediately, blanking an hourly-polled entity in Home Assistant until its
  // next poll. Go unavailable only after this many CONSECUTIVE read errors; a
  // successful publish resets the streak. Write errors never blank state (the
  // device value did not change) -- see VitoEntityBase::handle_write_error.
  static constexpr uint8_t NAN_AFTER_CONSECUTIVE_READ_ERRORS = 3;

  double scale_{1.0};
  bool signed_{false};
  bool big_endian_{false};
  // int16_t, not int8_t: a block can be up to 255 bytes and the selected byte
  // may sit past offset 127, so the signed offset needs the wider type to
  // still represent the -1 "disabled" sentinel unambiguously.
  int16_t extract_byte_{-1};
  uint8_t extract_len_{1};  // field width to slice at extract_byte_ (1..4)
  uint8_t consecutive_read_errors_{0};
};

}  // namespace esphome::vitohome
#endif  // USE_SENSOR
