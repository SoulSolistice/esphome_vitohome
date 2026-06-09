#pragma once
#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include "vito_uart_interface.h"
#include "vito_entity.h"

#include <VitoWiFi.h>

#include <deque>
#include <memory>
#include <vector>

namespace esphome {
namespace vitohome {

class VitoHomeComponent : public PollingComponent, public uart::UARTDevice {
 public:
  // ESPHomeUARTInterface stores `this` (as UARTDevice*); valid in the
  // member init list because base subobjects are already constructed.
  VitoHomeComponent() : iface_(this) {}

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void register_entity(VitoEntityBase *entity) {
    if (entity != nullptr) this->entities_.push_back(entity);
  }

 protected:
  // VitoWiFi takes free-function pointers for callbacks, so we route
  // them through a static instance pointer. Stage 1 enforces a single
  // VitoHomeComponent per device; setup() will mark_failed() if a
  // second one is constructed.
  static void on_response_(const VitoWiFi::PacketVS2 &response,
                           const VitoWiFi::Datapoint &request);
  static void on_error_(VitoWiFi::OptolinkResult error,
                        const VitoWiFi::Datapoint &request);
  static VitoHomeComponent *instance_;

  void validate_uart_();
  void dispatch_next_();

 private:
  ESPHomeUARTInterface iface_;
  // VitoWiFi's class template takes only the protocol version; the interface
  // type is deduced by the constructor (which wraps &iface_ in a
  // GenericInterface<ESPHomeUARTInterface> internally).
  std::unique_ptr<VitoWiFi::VitoWiFi<VitoWiFi::VS2>> vito_;

  std::vector<VitoEntityBase *> entities_;
  std::deque<VitoEntityBase *> queue_;
  VitoEntityBase *in_flight_{nullptr};
  uint32_t in_flight_started_ms_{0};

  // Failsafe: if a request is in flight for longer than this, log and
  // clear it. VitoWiFi has its own internal timeout (via OptolinkResult
  // ::TIMEOUT) but if a callback is somehow lost the queue stalls.
  static constexpr uint32_t IN_FLIGHT_WATCHDOG_MS = 10000;
};

}  // namespace vitohome
}  // namespace esphome
