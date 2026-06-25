#include "vito_sensor.h"

#include <cmath>

#include "decode.h"
#include "esphome/core/log.h"

namespace esphome {
namespace vitohome {

static const char *const TAG = "vitohome.sensor";

void VitoSensor::dump_config() {
  LOG_SENSOR("  ", "VitoHome Sensor", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X  Length: %u  scale: %g  signed: %s", this->datapoint_.address(),
                this->datapoint_.length(), this->scale_, this->signed_ ? "yes" : "no");
  if (this->extract_byte_ >= 0) {
    ESP_LOGCONFIG(TAG, "    Extract byte: %d", this->extract_byte_);
  }
}

void VitoSensor::handle_response(const ResponseView &response) {
  // Stage-2 decode path: vitohome bypasses the optolink engine's converters entirely
  // (their VariantValue is a tagless union and their math is float32 — see
  // decode.h and docs/stage2_design.md) and decodes the raw payload itself.
  const uint8_t *data = response.data;
  const uint8_t have = response.data_length;
  double value = NAN;
  bool ok;
  if (this->extract_byte_ >= 0) {
    ok = static_cast<uint8_t>(this->extract_byte_) < have &&
         decode_scaled(data + this->extract_byte_, 1, 1, this->signed_, this->scale_, &value);
  } else {
    ok = decode_scaled(data, have, this->datapoint_.length(), this->signed_, this->scale_, &value);
  }
  if (!ok) {
    ESP_LOGW(TAG, "%s: response too short (have %u bytes, need %u)", this->datapoint_.name(), have,
             this->extract_byte_ >= 0 ? this->extract_byte_ + 1 : this->datapoint_.length());
    return;
  }

  const float out = static_cast<float>(value);
  if (std::isnan(out) || std::isinf(out)) {
    ESP_LOGW(TAG, "%s: decoded non-finite value, skipping", this->datapoint_.name());
    return;
  }
  ESP_LOGD(TAG, "%s = %.3f", this->datapoint_.name(), out);
  this->publish_state(out);
}

void VitoSensor::handle_error(optolink::OptolinkResult /*error*/) {
  // Mark the entity unavailable in HA. The component logs the specific
  // error code; we just signal "no data" here.
  this->publish_state(NAN);
}

}  // namespace vitohome
}  // namespace esphome
