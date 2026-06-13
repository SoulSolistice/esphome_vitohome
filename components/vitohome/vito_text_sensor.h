#pragma once
#include <utility>
#include <vector>

#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "vito_entity.h"

namespace esphome {
namespace vitohome {

enum class TextSensorType : uint8_t {
  RAW_HEX,        // hex dump of the payload (debug / unknown structures)
  ENUM,           // map a raw integer to a label (read-only enums)
  ERROR_HISTORY,  // 9-byte error slot: [0]=code, [1..8]=DateTimeBCD
  DEVICE_ID,      // no bus reads of its own — fed by the hub's identification
};

class VitoTextSensor : public text_sensor::TextSensor, public Component, public VitoEntityBase {
 public:
  void set_type(TextSensorType type) { this->type_ = type; }
  // ENUM labels / ERROR_HISTORY code texts. Codegen feeds these one by one;
  // labels are string literals from codegen (static storage).
  void add_option(uint32_t value, const char *label) { this->options_.emplace_back(value, label); }

  void dump_config() override;
  void handle_response(const VitoWiFi::PacketVS2 &response) override;
  void handle_error(VitoWiFi::OptolinkResult error) override;
  const char *entity_kind() const override { return "text_sensor"; }

 protected:
  const char *lookup_(uint32_t value) const;
  void publish_raw_hex_(const uint8_t *data, uint8_t len);
  void publish_enum_(const uint8_t *data, uint8_t len);
  void publish_error_history_(const uint8_t *data, uint8_t len);

  TextSensorType type_{TextSensorType::RAW_HEX};
  std::vector<std::pair<uint32_t, const char *>> options_;
};

}  // namespace vitohome
}  // namespace esphome
