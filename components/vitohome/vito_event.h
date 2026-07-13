#pragma once
#include "esphome/core/defines.h"

#ifdef USE_EVENT
#include <utility>
#include <vector>

#include "esphome/components/event/event.h"
#include "esphome/core/component.h"
#include "vito_entity.h"

namespace esphome::vitohome {

// Fault-event entity: polls a fault-history slot (typically FA01, the newest
// fault, e.g. 0x7507 on the B3HA: code byte + 8-byte BCD timestamp) and fires
// a Home Assistant event when the code CHANGES -- a new fault fires its hex
// code ("0x10"), a cleared slot fires "cleared", a code outside the
// configured set fires "unknown" with the raw value in the log. This lands in
// HA's logbook natively and complements the polling error_history
// text_sensor, which shows the current slot contents but cannot notify.
//
// The FIRST successful poll only records a baseline and never fires:
// whatever fault happens to sit in the slot at boot would otherwise spam the
// logbook on every reboot.
class VitoEvent final : public event::Event, public Component, public VitoEntityBase {
 public:
  void add_code(uint32_t value, const char *label) { this->codes_.emplace_back(value, label); }

  void dump_config() override;
  void handle_response(const ResponseView &response) override;
  void handle_error(optolink::OptolinkResult error) override;
  const char *entity_kind() const override { return "event"; }

 protected:
  const char *label_for_(uint8_t code) const;

  std::vector<std::pair<uint32_t, const char *>> codes_;
  uint8_t last_code_{0};
  bool baseline_set_{false};
};

}  // namespace esphome::vitohome
#endif  // USE_EVENT
