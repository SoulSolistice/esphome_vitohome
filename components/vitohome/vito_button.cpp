#include "vito_button.h"
#ifdef USE_BUTTON

#include "esphome/core/log.h"
#include "vitohome.h"

namespace esphome::vitohome {

static const char *const TAG = "vitohome.button";

void VitoRefreshButton::dump_config() { LOG_BUTTON("  ", "Refresh Button", this); }

void VitoRefreshButton::press_action() {
  if (this->vh_parent_ == nullptr) {
    ESP_LOGE(TAG, "no hub bound");
    return;
  }
  this->vh_parent_->refresh_all();
}

void VitoActionButton::dump_config() {
  LOG_BUTTON("  ", "Action Button", this);
  ESP_LOGCONFIG(TAG, "    Procedure: 0x%04X, %u-byte parameter (remote procedure call)", this->datapoint_.address(),
                this->rpc_prefix_len());
}

void VitoActionButton::press_action() {
  if (this->vh_parent_ == nullptr) {
    ESP_LOGE(TAG, "%s: no hub bound", this->datapoint_.name());
    return;
  }
  ESP_LOGI(TAG, "%s: calling procedure 0x%04X", this->datapoint_.name(), this->datapoint_.address());
  if (!this->vh_parent_->request_write(this))
    ESP_LOGW(TAG, "%s: procedure call not queued", this->datapoint_.name());
}

void VitoActionButton::handle_write_response(const ResponseView &response) {
  ESP_LOGI(TAG, "%s: procedure acknowledged (%u byte%s returned)", this->datapoint_.name(), response.data_length,
           response.data_length == 1 ? "" : "s");
  // What the procedure changed is elsewhere -- a cleared history, values back
  // at their defaults -- so every value is read again.
  this->vh_parent_->refresh_all();
}

void VitoActionButton::handle_write_error(optolink::OptolinkResult error) {
  ESP_LOGW(TAG, "%s: procedure call failed (%s)", this->datapoint_.name(), optolink::errorToString(error));
}

}  // namespace esphome::vitohome
#endif  // USE_BUTTON
