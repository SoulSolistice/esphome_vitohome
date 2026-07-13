#pragma once
#include "esphome/core/defines.h"

#ifdef USE_TEXT_SENSOR
#include <utility>
#include <vector>

#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "vito_entity.h"

namespace esphome::vitohome {

enum class TextSensorType : uint8_t {
  RAW_HEX,        // hex dump of the payload (debug / unknown structures)
  ENUM,           // map a raw integer to a label (read-only enums)
  ERROR_HISTORY,  // 9-byte error slot: [0]=code, [1..8]=DateTimeBCD
  DEVICE_ID,      // no bus reads of its own — fed by the hub's identification
  ASCII,          // byte-array-as-string (HexByte2AsciiByte): Sachnummer etc.
  UTF16,          // UTF-16LE byte-string (HexByte2UTF16Byte): Beschriftung_HK1..3
  SCAN_RESULT,    // no bus reads of its own — fed by the hub's raw scan console
};

class VitoTextSensor final : public text_sensor::TextSensor, public Component, public VitoEntityBase {
 public:
  void set_type(TextSensorType type) { this->type_ = type; }
  // ENUM labels / ERROR_HISTORY code texts. Codegen feeds these one by one;
  // labels are string literals from codegen (static storage).
  void add_option(uint32_t value, const char *label) { this->options_.emplace_back(value, label); }
  // Aligned block extraction (read-only twin of the sensor's byte_offset):
  // `length` is a block read at the block BASE and the field is extract_len_
  // bytes at extract_byte_ inside it. Used by `enum` (1..4 bytes) and by the
  // string types `ascii` / `utf16` (up to 32 / 40 bytes). An interior read at
  // base+offset is NOT a substitute: P300 answers it with an error telegram and
  // KW answers it with 0xFF fill.
  void set_extract_byte(int16_t byte) { this->extract_byte_ = byte; }
  void set_extract_len(uint8_t len) { this->extract_len_ = len; }

  void dump_config() override;
  void handle_response(const ResponseView &response) override;
  void handle_error(optolink::OptolinkResult error) override;
  const char *entity_kind() const override { return "text_sensor"; }

 protected:
  const char *lookup_(uint32_t value) const;
  // Resolves (data, len) to the field span, honouring byte_offset/byte_length.
  // Returns false (and warns) if the field does not fit the response.
  bool slice_(const uint8_t *data, uint8_t len, const uint8_t *&field, uint8_t &width) const;
  void publish_raw_hex_(const uint8_t *data, uint8_t len);
  void publish_enum_(const uint8_t *data, uint8_t len);
  void publish_error_history_(const uint8_t *data, uint8_t len);
  void publish_ascii_(const uint8_t *data, uint8_t len);
  void publish_utf16_(const uint8_t *data, uint8_t len);

  TextSensorType type_{TextSensorType::RAW_HEX};
  std::vector<std::pair<uint32_t, const char *>> options_;
  int16_t extract_byte_{-1};
  uint8_t extract_len_{1};  // field width to slice at extract_byte_ (enum 1..4, ascii <=32, utf16 <=40)
};

}  // namespace esphome::vitohome
#endif  // USE_TEXT_SENSOR
