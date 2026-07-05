#include "vito_sensor.h"

#include <cmath>

#include "decode.h"
#include "esphome/core/log.h"

namespace esphome::vitohome {

static const char* const TAG = "vitohome.sensor";

void VitoSensor::dump_config() {
  LOG_SENSOR("  ", "VitoHome Sensor", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X  Length: %u  scale: %g  signed: %s", this->datapoint_.address(),
                this->datapoint_.length(), this->scale_, this->signed_ ? "yes" : "no");
  if (this->extract_byte_ >= 0) {
    ESP_LOGCONFIG(TAG, "    Extract: %u byte(s) at offset %d of a %u-byte block read", this->extract_len_,
                  this->extract_byte_, this->datapoint_.length());
  }
}

void VitoSensor::handle_response(const ResponseView& response) {
  // Stage-2 decode path: vitohome bypasses the optolink engine's converters entirely
  // (their VariantValue is a tagless union and their math is float32 — see
  // decode.h and docs/design_notes.md SS1) and decodes the raw payload itself.
  const uint8_t* data = response.data;
  const uint8_t have = response.data_length;
  double value = NAN;
  bool ok;
  if (this->extract_byte_ >= 0) {
    // Fetch the whole block (datapoint length), then scale/sign the field of
    // extract_len_ bytes at the offset. The bound check is against the bytes
    // actually received, so a short response fail-softs instead of reading
    // past the end. Little-endian only: every extracted field in the Vitosoft
    // data is LE (the big-endian RotateBytes converter never uses extraction).
    const uint16_t off = static_cast<uint16_t>(this->extract_byte_);
    ok = off + this->extract_len_ <= have &&
         decode_scaled(data + off, this->extract_len_, this->extract_len_, this->signed_, this->scale_, &value);
  } else {
    ok = this->big_endian_
             ? decode_scaled_be(data, have, this->datapoint_.length(), this->signed_, this->scale_, &value)
             : decode_scaled(data, have, this->datapoint_.length(), this->signed_, this->scale_, &value);
  }
  if (!ok) {
    ESP_LOGW(TAG, "%s: response too short (have %u bytes, need %u)", this->datapoint_.name(), have,
             this->extract_byte_ >= 0 ? static_cast<unsigned>(this->extract_byte_ + this->extract_len_)
                                      : this->datapoint_.length());
    return;
  }

  const float out = static_cast<float>(value);
  if (std::isnan(out) || std::isinf(out)) {
    ESP_LOGW(TAG, "%s: decoded non-finite value, skipping", this->datapoint_.name());
    return;
  }
  ESP_LOGD(TAG, "%s = %.3f", this->datapoint_.name(), out);
  this->consecutive_read_errors_ = 0;
  this->publish_state(out);
}

void VitoSensor::handle_error(optolink::OptolinkResult /*error*/) {
  // Mark the entity unavailable in HA -- but only after a streak of failed
  // reads. The component logs the specific error code; we just signal
  // "no data" here once the streak crosses the threshold.
  if (this->consecutive_read_errors_ < NAN_AFTER_CONSECUTIVE_READ_ERRORS) {
    this->consecutive_read_errors_++;
  }
  if (this->consecutive_read_errors_ == NAN_AFTER_CONSECUTIVE_READ_ERRORS) {
    this->publish_state(NAN);
  }
}

}  // namespace esphome::vitohome
