#pragma once
#include "esphome/core/defines.h"

#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#include "esphome/core/component.h"

namespace esphome::vitohome {

class VitoHomeComponent;

// Force-refresh button: pressing it calls the hub's refresh_all(), which
// marks every registered datapoint due on the next scheduler tick (reads
// only; the existing queue discipline throttles the burst). Usable from the
// HA UI and from automations via button.press; ESPHome-side automations can
// call id(vito).refresh_all() directly instead.
class VitoRefreshButton : public button::Button, public Component {
 public:
  void set_vitohome_parent(VitoHomeComponent *parent) { this->vh_parent_ = parent; }
  void dump_config() override;

 protected:
  void press_action() override;

  VitoHomeComponent *vh_parent_{nullptr};
};

}  // namespace esphome::vitohome
#endif  // USE_BUTTON
