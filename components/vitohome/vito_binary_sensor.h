#pragma once
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/core/component.h"
#include "vito_entity.h"

namespace esphome::vitohome {

class VitoBinarySensor : public binary_sensor::BinarySensor, public Component, public VitoEntityBase {
 public:
  void set_byte_offset(uint8_t offset) { this->byte_offset_ = offset; }
  void set_bit_mask(uint8_t mask) { this->bit_mask_ = mask; }

  void dump_config() override;
  void handle_response(const ResponseView &response) override;
  void handle_error(optolink::OptolinkResult error) override;
  const char *entity_kind() const override { return "binary_sensor"; }

 protected:
  uint8_t byte_offset_{0};
  uint8_t bit_mask_{0xFF};
};

}  // namespace esphome::vitohome
