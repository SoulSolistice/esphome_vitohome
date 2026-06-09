#include "vito_select.h"

#include "decode.h"
#include "esphome/core/log.h"
#include "vitohome.h"

namespace esphome {
namespace vitohome {

static const char *const TAG = "vitohome.select";

void VitoSelect::dump_config() {
  LOG_SELECT("  ", "VitoHome Select", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X  Length: %u  Options: %zu", this->datapoint_.address(),
                this->datapoint_.length(), this->raw_values_.size());
}

void VitoSelect::control(size_t index) {
  if (index >= this->raw_values_.size()) {
    ESP_LOGE(TAG, "%s: index %zu out of range", this->datapoint_.name(), index);
    return;
  }
  const uint32_t raw = this->raw_values_[index];
  const uint8_t len = this->get_write_datapoint().length();
  uint8_t buf[4];
  for (uint8_t i = 0; i < len && i < 4; i++) {
    buf[i] = static_cast<uint8_t>((raw >> (8 * i)) & 0xFF);
  }
  if (!this->set_write_payload_(buf, len)) {
    ESP_LOGE(TAG, "%s: failed to stage write payload", this->datapoint_.name());
    return;
  }
  this->pending_index_ = index;
  if (this->vh_parent_ == nullptr || !this->vh_parent_->request_write(this)) {
    ESP_LOGE(TAG, "%s: write could not be queued", this->datapoint_.name());
    return;
  }
  ESP_LOGD(TAG, "%s: queued write option %zu (raw 0x%02X)", this->datapoint_.name(), index, raw);
}

void VitoSelect::handle_response(const VitoWiFi::PacketVS2 &response) {
  const uint8_t len = this->datapoint_.length();
  if (response.dataLength() < len) {
    ESP_LOGW(TAG, "%s: response too short (have %u bytes, need %u)", this->datapoint_.name(), response.dataLength(),
             len);
    return;
  }
  const uint32_t raw = static_cast<uint32_t>(read_le(response.data(), len > 4 ? 4 : len));
  for (size_t i = 0; i < this->raw_values_.size(); i++) {
    if (this->raw_values_[i] == raw) {
      ESP_LOGD(TAG, "%s = option %zu (raw 0x%02X)", this->datapoint_.name(), i, raw);
      this->publish_state(i);
      return;
    }
  }
  // ESPHome selects can only publish mapped options; an unmapped wire value
  // stays unpublished and is surfaced in the log instead.
  ESP_LOGW(TAG, "%s: device value 0x%02X is not in the configured options", this->datapoint_.name(), raw);
}

void VitoSelect::handle_write_response(const VitoWiFi::PacketVS2 & /*response*/) {
  if (!this->read_back_) {
    this->publish_state(this->pending_index_);
  }
}

void VitoSelect::handle_error(VitoWiFi::OptolinkResult /*error*/) {
  // Keep the last option; the hub logs the specific error.
}

}  // namespace vitohome
}  // namespace esphome
