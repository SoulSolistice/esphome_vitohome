#include "vito_event.h"

#include <cstdio>

#include "esphome/core/log.h"
#include "vitohome.h"

namespace esphome::vitohome {

static const char* const TAG = "vitohome.event";

void VitoEvent::dump_config() {
  LOG_EVENT("  ", "VitoHome Event", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X  Length: %u  Codes: %zu", this->datapoint_.address(),
                this->datapoint_.length(), this->codes_.size());
}

const char* VitoEvent::label_for_(uint8_t code) const {
  for (const auto& entry : this->codes_) {
    if (entry.first == code) return entry.second;
  }
  return nullptr;
}

void VitoEvent::handle_response(const ResponseView& response) {
  if (response.data_length < 1 || response.data == nullptr) {
    ESP_LOGW(TAG, "%s: empty response", this->datapoint_.name());
    return;
  }
  const uint8_t code = response.data[0];
  if (!this->baseline_set_) {
    // Never fire for whatever fault already sits in the slot at boot -- that
    // would spam the HA logbook on every reboot. Record and wait for change.
    this->baseline_set_ = true;
    this->last_code_ = code;
    ESP_LOGD(TAG, "%s: baseline fault code 0x%02X", this->datapoint_.name(), code);
    return;
  }
  if (code == this->last_code_) return;
  this->last_code_ = code;
  if (code == 0x00) {
    ESP_LOGI(TAG, "%s: fault cleared", this->datapoint_.name());
    this->trigger("cleared");
    return;
  }
  const char* label = this->label_for_(code);
  if (label != nullptr) {
    char type_buf[8];
    std::snprintf(type_buf, sizeof(type_buf), "0x%02X", code);
    ESP_LOGW(TAG, "%s: new fault 0x%02X (%s)", this->datapoint_.name(), code, label);
    this->trigger(type_buf);
  } else {
    ESP_LOGW(TAG, "%s: new fault 0x%02X (not in configured codes)", this->datapoint_.name(), code);
    this->trigger("unknown");
  }
}

void VitoEvent::handle_error(optolink::OptolinkResult /*error*/) {
  // Keep the baseline; the hub logs the specific error. A read glitch must
  // not manufacture fault events.
}

}  // namespace esphome::vitohome
