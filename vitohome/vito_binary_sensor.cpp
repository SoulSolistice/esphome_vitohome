#include "vito_binary_sensor.h"
#include "esphome/core/log.h"

namespace esphome {
namespace vitohome {

static const char *const TAG = "vitohome.binary_sensor";

void VitoBinarySensor::dump_config() {
  LOG_BINARY_SENSOR("  ", "Vitoconnect Binary Sensor", this);
  ESP_LOGCONFIG(TAG,
                "    Address: 0x%04X  Length: %u  byte_offset: %u  bit_mask: 0x%02X",
                this->datapoint_.address(), this->datapoint_.length(),
                this->byte_offset_, this->bit_mask_);
}

void VitoBinarySensor::handle_response(const VitoWiFi::PacketVS2 &response) {
  // For binary sensors we read raw bytes rather than going through the
  // VitoWiFi converter, since the truthiness depends on a configurable
  // (byte_offset, bit_mask) within the response payload.
  if (response.dataLength() <= this->byte_offset_) {
    ESP_LOGW(TAG, "%s: response too short (have %u bytes, need offset %u)",
             this->datapoint_.name(),
             response.dataLength(), this->byte_offset_);
    return;
  }
  uint8_t raw = response.data()[this->byte_offset_];
  bool value = (raw & this->bit_mask_) != 0;
  ESP_LOGD(TAG, "%s: raw=0x%02X mask=0x%02X -> %s",
           this->datapoint_.name(), raw, this->bit_mask_,
           value ? "ON" : "OFF");
  this->publish_state(value);
}

void VitoBinarySensor::handle_error(VitoWiFi::OptolinkResult /*error*/) {
  // ESPHome binary_sensor has no native "unavailable" state; we leave
  // the last value in place and rely on the component to log the error.
}

}  // namespace vitohome
}  // namespace esphome