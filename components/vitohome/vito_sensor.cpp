#include "vito_sensor.h"

#include <cmath>

#include "esphome/core/log.h"

namespace esphome {
namespace vitohome {

static const char *const TAG = "vitohome.sensor";

void VitoSensor::dump_config() {
  LOG_SENSOR("  ", "Vitoconnect Sensor", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X  Length: %u", this->datapoint_.address(), this->datapoint_.length());
}

void VitoSensor::handle_response(const VitoWiFi::PacketVS2 &response) {
  // VitoWiFi's VariantValue is a non-discriminated union with type-specific
  // out-operators. The div* converters store a float, but noconv stores an
  // *unsigned integer* member sized by length. Calling operator float() on a
  // noconv value would read the float member of the union over integer bytes
  // and yield garbage, so select the operator that matches the converter.
  const auto &dp = this->datapoint_;
  const VitoWiFi::VariantValue v = dp.decode(response);

  float value;
  if (dp.converter() == VitoWiFi::noconv) {
    switch (dp.length()) {
      case 1:
        value = static_cast<float>(static_cast<uint8_t>(v));
        break;
      case 2:
        value = static_cast<float>(static_cast<uint16_t>(v));
        break;
      case 4:
        value = static_cast<float>(static_cast<uint32_t>(v));
        break;
      default:
        value = NAN;
        break;  // schema restricts length to 1/2/4
    }
  } else {
    value = static_cast<float>(v);  // div10 / div2 / div3600 -> float member
  }

  if (std::isnan(value) || std::isinf(value)) {
    ESP_LOGW(TAG, "%s: decoded non-finite value, skipping", dp.name());
    return;
  }
  ESP_LOGD(TAG, "%s = %.3f", dp.name(), value);
  this->publish_state(value);
}

void VitoSensor::handle_error(VitoWiFi::OptolinkResult /*error*/) {
  // Mark the entity unavailable in HA. The component logs the specific
  // error code; we just signal "no data" here.
  this->publish_state(NAN);
}

}  // namespace vitohome
}  // namespace esphome
