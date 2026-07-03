#include "vito_button.h"

#include "esphome/core/log.h"
#include "vitohome.h"

namespace esphome::vitohome {

static const char* const TAG = "vitohome.button";

void VitoRefreshButton::dump_config() { LOG_BUTTON("  ", "VitoHome Refresh Button", this); }

void VitoRefreshButton::press_action() {
  if (this->vh_parent_ == nullptr) {
    ESP_LOGE(TAG, "no hub bound");
    return;
  }
  this->vh_parent_->refresh_all();
}

}  // namespace esphome::vitohome
