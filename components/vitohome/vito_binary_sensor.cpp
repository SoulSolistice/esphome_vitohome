#include "vito_binary_sensor.h"

#include "decode.h"
#include "esphome/core/log.h"

namespace esphome {
namespace vitohome {

static const char *const TAG = "vitohome.binary_sensor";

void VitoBinarySensor::dump_config() {
  LOG_BINARY_SENSOR("  ", "VitoHome Binary Sensor", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X  Length: %u  byte_offset: %u  bit_mask: 0x%02X", this->datapoint_.address(),
                this->datapoint_.length(), this->byte_offset_, this->bit_mask_);
}

void VitoBinarySensor::handle_response(const ResponseView &response) {
  // Raw-byte read: truthiness is a configurable (byte_offset, bit_mask)
  // within the payload, so we bypass the optolink converter. The range
  // check and extraction live in decode_masked_bit() so they can be
  // unit-tested on the host.
  bool value;
  if (!decode_masked_bit(response.data, response.data_length, this->byte_offset_, this->bit_mask_, &value)) {
    ESP_LOGW(TAG, "%s: response too short (have %u bytes, need offset %u)", this->datapoint_.name(),
             response.data_length, this->byte_offset_);
    return;
  }
  uint8_t raw = response.data[this->byte_offset_];
  ESP_LOGD(TAG, "%s: raw=0x%02X mask=0x%02X -> %s", this->datapoint_.name(), raw, this->bit_mask_,
           value ? "ON" : "OFF");
  this->publish_state(value);
}

void VitoBinarySensor::handle_error(optolink::OptolinkResult /*error*/) {
  // ESPHome binary_sensor has no native "unavailable" state; we leave
  // the last value in place and rely on the component to log the error.
}

}  // namespace vitohome
}  // namespace esphome
