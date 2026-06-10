#pragma once
#include <VitoWiFi.h>

namespace esphome {
namespace vitohome {

// Common base for any entity that owns a VitoWiFi datapoint. The
// component holds a vector<VitoEntityBase*> and dispatches read
// responses/errors back to the originating entity via the in_flight_
// pointer. Concrete subclasses translate VitoWiFi packets into ESPHome
// state publishes.
class VitoEntityBase {
 public:
  virtual ~VitoEntityBase() = default;

  void set_datapoint(const VitoWiFi::Datapoint &dp) { this->datapoint_ = dp; }
  const VitoWiFi::Datapoint &get_datapoint() const { return this->datapoint_; }

  // Called by the component on a successful response. The packet
  // length and validity have already been checked by VitoWiFi.
  virtual void handle_response(const VitoWiFi::PacketVS2 &response) = 0;

  // Called by the component on a protocol-level error. Concrete
  // entities typically publish an "unavailable" indicator.
  virtual void handle_error(VitoWiFi::OptolinkResult error) = 0;

  // For logging / dump_config.
  virtual const char *entity_kind() const = 0;

 protected:
  // Default-constructed Datapoint until set_datapoint runs from codegen.
  VitoWiFi::Datapoint datapoint_{"uninitialized", 0, 1, VitoWiFi::noconv};
};

}  // namespace vitohome
}  // namespace esphome