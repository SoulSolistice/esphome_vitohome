#pragma once
#include "esphome/core/defines.h"

#ifdef USE_TEXT_SENSOR
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
  // ENUM labels / ERROR_HISTORY code texts, as a codegen-emitted static table
  // in .rodata (see VitoOption). Pointer + count only: no heap, and no
  // per-row statement in the generated setup().
  void set_options(const VitoOption *options, uint16_t count) {
    this->options_ = options;
    this->option_count_ = count;
  }

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
  const VitoOption *options_{nullptr};
  uint16_t option_count_{0};
};

}  // namespace esphome::vitohome
#endif  // USE_TEXT_SENSOR
