#include "vito_text_sensor.h"
#ifdef USE_TEXT_SENSOR

#include <cstdio>
#include <string>

#include "decode.h"
#include "esphome/core/log.h"

namespace esphome::vitohome {

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
    case TextSensorType::UTF16:
      return "utf16";
    case TextSensorType::SCAN_RESULT:
      return "scan_result";
  }
  return "?";
}

void VitoTextSensor::dump_config() {
  LOG_TEXT_SENSOR("  ", "VitoHome Text Sensor", this);
  if (this->type_ == TextSensorType::DEVICE_ID) {
    ESP_LOGCONFIG(TAG, "    Type: device_id (fed by hub identification)");
    return;
  }
  if (this->type_ == TextSensorType::SCAN_RESULT) {
    ESP_LOGCONFIG(TAG, "    Type: scan_result (fed by hub raw scan console)");
    return;
  }
  ESP_LOGCONFIG(TAG, "    Type: %s  Address: 0x%04X  Length: %u", type_name(this->type_), this->datapoint_.address(),
                this->datapoint_.length());
}

const char *VitoTextSensor::lookup_(uint32_t value) const {
  for (const auto &kv : this->options_) {
    if (kv.first == value)
      return kv.second;
  }
  return nullptr;
}

void VitoTextSensor::publish_raw_hex_(const uint8_t *data, uint8_t len) {
  std::string out;
  out.reserve(static_cast<size_t>(len) * 3);
  char b[4];
  for (uint8_t i = 0; i < len; i++) {
    snprintf(b, sizeof(b), "%02X", data[i]);
    if (i != 0)
      out += ' ';
    out += b;
  }
  this->publish_state(out);
}

void VitoTextSensor::publish_enum_(const uint8_t *data, uint8_t len) {
  // With extraction the response is the whole block read at the block base;
  // the enum field is extract_len_ bytes at extract_byte_ (bound-checked
  // against the bytes actually received, like the other extracting entities).
  if (this->extract_byte_ >= 0) {
    const uint16_t off = static_cast<uint16_t>(this->extract_byte_);
    if (off + this->extract_len_ > len) {
      ESP_LOGW(TAG, "%s: response too short (have %u bytes, need %u)", this->datapoint_.name(), len,
               static_cast<unsigned>(off + this->extract_len_));
      return;
    }
    data += off;
    len = this->extract_len_;
  }
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

bool VitoTextSensor::slice_(const uint8_t *data, uint8_t len, const uint8_t *&field, uint8_t &width) const {
  // Without byte_offset the whole response IS the field (the historical shape).
  // With byte_offset, `length` was a block read at the block base and the field
  // is extract_len_ bytes at extract_byte_ inside it -- the same aligned-read
  // discipline sensor/binary_sensor/enum already use, and the reason the string
  // types now need it: Beschriftung_HK1~0x7360 is BlockLength 42, BytePosition
  // 2, ByteLength 40. Reading it at 0x7360+2 is an INTERIOR read; P300 answers
  // that with an error telegram regardless of the byte count, and KW answers it
  // with 0xFF fill. Hardware-confirmed 2026-07-10 (a 2-byte read at 0x7362 and
  // a 40-byte read at 0x7362 fail identically).
  field = data;
  width = len;
  if (this->extract_byte_ < 0)
    return true;
  const uint16_t off = static_cast<uint16_t>(this->extract_byte_);
  if (off + this->extract_len_ > len) {
    ESP_LOGW(TAG, "%s: byte_offset %u + byte_length %u exceeds response (%u bytes)", this->datapoint_.name(),
             static_cast<unsigned>(off), static_cast<unsigned>(this->extract_len_), static_cast<unsigned>(len));
    return false;
  }
  field = data + off;
  width = this->extract_len_;
  return true;
}

void VitoTextSensor::publish_ascii_(const uint8_t *data, uint8_t len) {
  // HexByte2AsciiByte: the payload is an ASCII byte-string (device part /
  // serial number). decode_ascii() NUL-terminates, trims trailing spaces and
  // maps non-printable bytes to '?'. Cap at 32 chars (longest such field is
  // the 16-byte Herstellnummer).
  const uint8_t *field;
  uint8_t width;
  if (!this->slice_(data, len, field, width))
    return;
  const uint8_t use = width > 32 ? 32 : width;
  char buf[40];
  if (decode_ascii(field, width, use, buf, sizeof(buf)) < 0) {
    ESP_LOGW(TAG, "%s: ASCII decode failed (len=%u)", this->datapoint_.name(), width);
    return;
  }
  ESP_LOGD(TAG, "%s: \"%s\"", this->datapoint_.name(), buf);
  this->publish_state(buf);
}

void VitoTextSensor::publish_utf16_(const uint8_t *data, uint8_t len) {
  // HexByte2UTF16Byte: a UTF-16LE label (Beschriftung_HK1..3, 40 bytes = 20
  // code units). decode_utf16() emits UTF-8, NUL-terminates, trims trailing
  // spaces and skips 0xFFFF fill. Cap at 40 bytes; worst case is 3 UTF-8 bytes
  // per code unit (60) + NUL.
  const uint8_t *field;
  uint8_t width;
  if (!this->slice_(data, len, field, width))
    return;
  const uint8_t use = width > 40 ? 40 : width;
  char buf[80];
  if (decode_utf16(field, width, use, buf, sizeof(buf)) < 0) {
    ESP_LOGW(TAG, "%s: UTF-16 decode failed (len=%u)", this->datapoint_.name(), width);
    return;
  }
  ESP_LOGD(TAG, "%s: \"%s\"", this->datapoint_.name(), buf);
  this->publish_state(buf);
}

void VitoTextSensor::handle_response(const ResponseView &response) {
  const uint8_t *data = response.data;
  const uint8_t len = response.data_length;
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
    case TextSensorType::SCAN_RESULT:
      // Never polled — fed by the hub's raw-scan lane. Nothing to do here.
      return;
    case TextSensorType::ASCII:
      this->publish_ascii_(data, len);
      return;
    case TextSensorType::UTF16:
      this->publish_utf16_(data, len);
      return;
  }
}

void VitoTextSensor::handle_error(optolink::OptolinkResult /*error*/) {
  // Keep the last value; the hub logs the specific error.
}

}  // namespace esphome::vitohome
#endif  // USE_TEXT_SENSOR
