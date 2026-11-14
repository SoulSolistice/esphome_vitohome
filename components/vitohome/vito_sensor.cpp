#include "vito_sensor.h"
#ifdef USE_SENSOR

#include <cmath>

#include "decode.h"
#include "esphome/core/log.h"

namespace esphome::vitohome {

static const char *const TAG = "vitohome.sensor";

void VitoSensor::dump_config() {
  LOG_SENSOR("  ", "Sensor", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X  Length: %u  scale: %g  signed: %s", this->datapoint_.address(),
                this->datapoint_.length(), this->scale_, this->signed_ ? "yes" : "no");
  if (this->extract_byte_ >= 0) {
    ESP_LOGCONFIG(TAG, "    Extract: %u byte(s) at offset %d of a %u-byte block read", this->extract_len_,
                  this->extract_byte_, this->datapoint_.length());
  }
  if (this->field_mask_ != 0) {
    ESP_LOGCONFIG(TAG, "    Bit mask: 0x%02X", this->field_mask_);
  }
}

void VitoSensor::handle_response(const ResponseView &response) {
  // vitohome decodes the raw payload itself (decode.h; design_notes.md SS1).
  // A WRITE frame can arrive with data == nullptr and a non-zero length
  // (PacketVS2::data()).
  if (response.data == nullptr) {
    ESP_LOGW(TAG, "%s: null response payload", this->datapoint_.name());
    return;
  }
  const uint8_t width = this->field_width_();
  double value = NAN;
  bool ok;
  if (this->field_mask_ != 0) {
    // Unsigned: sensor.py rejects `signed` with bit_mask.
    uint32_t field = 0;
    ok = this->field_value_(response, &field);
    const uint8_t bits = static_cast<uint8_t>(field);
    ok = ok && decode_scaled(&bits, 1, 1, false, this->scale_, &value);
  } else {
    // Big-endian only without extraction: sensor.py rejects it with byte_offset.
    const uint8_t *field = nullptr;
    ok = this->field_bytes_(response, &field) &&
         (this->big_endian_ ? decode_scaled_be(field, width, width, this->signed_, this->scale_, &value)
                            : decode_scaled(field, width, width, this->signed_, this->scale_, &value));
  }
  if (!ok) {
    ESP_LOGW(TAG, "%s: response too short (have %u bytes, need %u)", this->datapoint_.name(), response.data_length,
             this->field_end_());
    this->note_read_failure_();
    return;
  }

  const float out = static_cast<float>(value);
  if (std::isnan(out) || std::isinf(out)) {
    ESP_LOGW(TAG, "%s: decoded non-finite value, skipping", this->datapoint_.name());
    this->note_read_failure_();
    return;
  }
  ESP_LOGD(TAG, "%s = %.3f", this->datapoint_.name(), out);
  this->clear_read_failures_();
  this->publish_state(out);
}

void VitoSensor::note_read_failure_() {
  if (this->count_read_failure_())
    this->publish_state(NAN);
}

void VitoSensor::handle_error(optolink::OptolinkResult /*error*/) { this->note_read_failure_(); }

}  // namespace esphome::vitohome
#endif  // USE_SENSOR
