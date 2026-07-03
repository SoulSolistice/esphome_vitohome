#include "vito_switch.h"

#include "decode.h"
#include "esphome/core/log.h"
#include "vitohome.h"

namespace esphome {
namespace vitohome {

static const char* const TAG = "vitohome.switch";

void VitoSwitch::dump_config() {
  LOG_SWITCH("  ", "VitoHome Switch", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X  Length: %u  On: 0x%02X  Off: 0x%02X", this->datapoint_.address(),
                this->datapoint_.length(), this->on_value_, this->off_value_);
}

void VitoSwitch::write_state(bool state) {
  const uint32_t raw = state ? this->on_value_ : this->off_value_;
  const uint8_t len = this->get_write_datapoint().length();
  uint8_t buf[4];
  for (uint8_t i = 0; i < len && i < 4; i++) {
    buf[i] = static_cast<uint8_t>((raw >> (8 * i)) & 0xFF);
  }
  if (!this->set_write_payload_(buf, len)) {
    ESP_LOGE(TAG, "%s: failed to stage write payload", this->datapoint_.name());
    return;
  }
  this->pending_state_ = state;
  if (this->vh_parent_ == nullptr || !this->vh_parent_->request_write(this)) {
    ESP_LOGE(TAG, "%s: write could not be queued", this->datapoint_.name());
    return;
  }
  // Not optimistic: like VitoSelect, the published state changes on read-back
  // (or on the write ack when read_back is off), never on the queue action.
  ESP_LOGD(TAG, "%s: queued write %s (raw 0x%02X)", this->datapoint_.name(), ONOFF(state), raw);
}

void VitoSwitch::handle_response(const ResponseView& response) {
  const uint8_t len = this->datapoint_.length();
  if (response.data_length < len) {
    ESP_LOGW(TAG, "%s: response too short (have %u bytes, need %u)", this->datapoint_.name(), response.data_length,
             len);
    return;
  }
  const uint32_t raw = static_cast<uint32_t>(read_le(response.data, len > 4 ? 4 : len));
  for (uint32_t on : this->on_state_values_) {
    if (raw == on) {
      ESP_LOGD(TAG, "%s = ON (raw 0x%02X)", this->datapoint_.name(), raw);
      this->publish_state(true);
      return;
    }
  }
  if (raw == this->off_value_) {
    ESP_LOGD(TAG, "%s = OFF (raw 0x%02X)", this->datapoint_.name(), raw);
    this->publish_state(false);
    return;
  }
  // Same policy as VitoSelect for unmapped wire values: keep the last state
  // and surface the raw value in the log (add it to on_values if it means on).
  ESP_LOGW(TAG, "%s: device value 0x%02X is neither an on_values entry nor off_value", this->datapoint_.name(), raw);
}

void VitoSwitch::handle_write_response(const ResponseView& /*response*/) {
  if (!this->read_back_) {
    this->publish_state(this->pending_state_);
  }
}

void VitoSwitch::handle_error(optolink::OptolinkResult /*error*/) {
  // Keep the last state; the hub logs the specific error.
}

}  // namespace vitohome
}  // namespace esphome
