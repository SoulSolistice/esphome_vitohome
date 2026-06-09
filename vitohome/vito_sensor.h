#pragma once
#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "vito_entity.h"

namespace esphome {
namespace vitohome {

class VitoSensor : public sensor::Sensor, public Component, public VitoEntityBase {
 public:
  void dump_config() override;
  void handle_response(const VitoWiFi::PacketVS2 &response) override;
  void handle_error(VitoWiFi::OptolinkResult error) override;
  const char *entity_kind() const override { return "sensor"; }
};

}  // namespace vitohome
}  // namespace esphome