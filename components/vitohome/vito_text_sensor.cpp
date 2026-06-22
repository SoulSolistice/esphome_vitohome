#include "vito_text_sensor.h"

#include <cstdio>
#include <string>

#include "decode.h"
#include "esphome/core/log.h"

namespace esphome {
namespace vitohome {

static const char *const TAG = "vitohome.text_sensor";

static const char *type_name(TextSensorType t) {
  switch (t) {
    case TextSensorType::RAW_HEX:
      return "raw";
    case TextSensorType::ENUM:
      return "enum";
    case TextSensorType::ERROR_HISTORY:
      return "error_history";
    case TextSensorType::DEVICE_ID:
      return "device_id";
    case TextSensorType::ASCII:
      return "ascii";
  }
  return "?";
}

void VitoTextSensor::dump_config() {
  LOG_TEXT_SENSOR("  ", "VitoHome Text Sensor", this);
  if (this->type_ == TextSensorType::DEVICE_ID) {
    ESP_LOGCONFIG(TAG, "    Type: device_id (fed by hub identification)");
    return;
  }
  ESP_LOGCONFIG(TAG, "    Type: %s  Address: 0x%04X  Length: %u", type_name(this->type_), this->datapoint_.address(),
                this->datapoint_.length());
}

const char *VitoTextSensor::lookup_(uint32_t value) const {
  for (const auto &kv : this->options_) {
    if (kv.first == value) return kv.second;
  }
  return nullptr;
}

void VitoTextSensor::publish_raw_hex_(const uint8_t *data, uint8_t len) {
  std::string out;
  out.reserve(static_cast<size_t>(len) * 3);
  char b[4];
  for (uint8_t i = 0; i < len; i++) {
    snprintf(b, sizeof(b), "%02X", data[i]);
    if (i != 0) out += ' ';
    out += b;
  }
  this->publish_state(out);
}

void VitoTextSensor::publish_enum_(const uint8_t *data, uint8_t len) {
  const uint8_t use = len > 4 ? 4 : len;
  const uint32_t raw = static_cast<uint32_t>(read_le(data, use));
  const char *label = this->lookup_(raw);
  if (label != nullptr) {
    this->publish_state(label);
    return;
  }
  char buf[24];
  snprintf(buf, sizeof(buf), "Unbekannt (0x%02X)", raw);
  ESP_LOGW(TAG, "%s: value 0x%02X has no mapped option", this->datapoint_.name(), raw);
  this->publish_state(buf);
}

void VitoTextSensor::publish_error_history_(const uint8_t *data, uint8_t len) {
  // Layout (InsideViessmannVitosoft, Viessmann2MQTT.py): a 9-byte slot is
  // [0] = error code, [1..8] = DateTimeBCD (year-hi, year-lo, month, day,
  // weekday, hour, minute, second). Empty slots are 0xFF-filled, which
  // fails the BCD validity check -> code-only output.
  if (len < 1) {
    ESP_LOGW(TAG, "%s: empty error-history response", this->datapoint_.name());
    return;
  }
  const uint8_t code = data[0];
  const char *text = this->lookup_(code);

  BcdDateTime dt{};
  const bool has_dt = decode_datetime_bcd(data, len, 1, &dt);

  char buf[160];
  if (text != nullptr && has_dt) {
    snprintf(buf, sizeof(buf), "%s (0x%02X) @ %04u-%02u-%02u %02u:%02u:%02u", text, code, dt.year, dt.month, dt.day,
             dt.hour, dt.minute, dt.second);
  } else if (text != nullptr) {
    snprintf(buf, sizeof(buf), "%s (0x%02X)", text, code);
  } else if (has_dt) {
    snprintf(buf, sizeof(buf), "Fehler 0x%02X @ %04u-%02u-%02u %02u:%02u:%02u", code, dt.year, dt.month, dt.day,
             dt.hour, dt.minute, dt.second);
  } else {
    snprintf(buf, sizeof(buf), "Fehler 0x%02X", code);
  }
  ESP_LOGD(TAG, "%s: %s", this->datapoint_.name(), buf);
  this->publish_state(buf);
}

void VitoTextSensor::publish_ascii_(const uint8_t *data, uint8_t len) {
  // HexByte2AsciiByte: the payload is an ASCII byte-string (device part /
  // serial number). decode_ascii() NUL-terminates, trims trailing spaces and
  // maps non-printable bytes to '?'. Cap at 32 chars (longest such field is
  // the 16-byte Herstellnummer).
  const uint8_t use = len > 32 ? 32 : len;
  char buf[40];
  if (decode_ascii(data, len, use, buf, sizeof(buf)) < 0) {
    ESP_LOGW(TAG, "%s: ASCII decode failed (len=%u)", this->datapoint_.name(), len);
    return;
  }
  ESP_LOGD(TAG, "%s: \"%s\"", this->datapoint_.name(), buf);
  this->publish_state(buf);
}

void VitoTextSensor::handle_response(const optolink::PacketVS2 &response) {
  const uint8_t *data = response.data();
  const uint8_t len = response.dataLength();
  if (data == nullptr || len == 0) {
    ESP_LOGW(TAG, "%s: empty response", this->datapoint_.name());
    return;
  }
  switch (this->type_) {
    case TextSensorType::RAW_HEX:
      this->publish_raw_hex_(data, len);
      return;
    case TextSensorType::ENUM:
      this->publish_enum_(data, len);
      return;
    case TextSensorType::ERROR_HISTORY:
      this->publish_error_history_(data, len);
      return;
    case TextSensorType::DEVICE_ID:
      // Never polled — fed by the hub. Nothing to do.
      return;
    case TextSensorType::ASCII:
      this->publish_ascii_(data, len);
      return;
  }
}

void VitoTextSensor::handle_error(optolink::OptolinkResult /*error*/) {
  // Keep the last value; the hub logs the specific error.
}

}  // namespace vitohome
}  // namespace esphome
