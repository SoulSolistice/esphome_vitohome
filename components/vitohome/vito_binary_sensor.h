#pragma once
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/core/component.h"
#include "vito_entity.h"

namespace esphome {
namespace vitohome {

class VitoBinarySensor : public binary_sensor::BinarySensor, public Component, public VitoEntityBase {
 public:
  void set_byte_offset(uint8_t v) { byte_offset_ = v; }
  void set_bit_mask(uint8_t v) { bit_mask_ = v; }

  void dump_config() override;
  void handle_response(const VitoWiFi::PacketVS2 &response) override;
  void handle_error(VitoWiFi::OptolinkResult error) override;
  const char *entity_kind() const override { return "binary_sensor"; }

 private:
  uint8_t byte_offset_{0};
  uint8_t bit_mask_{0xFF};
};

}  // namespace vitohome
}  // namespace esphome
