#include "vito_select.h"
#ifdef USE_SELECT

#include <cinttypes>

#include "decode.h"
#include "esphome/core/log.h"
#include "vitohome.h"

namespace esphome::vitohome {

static const char *const TAG = "vitohome.select";

void VitoSelect::dump_config() {
  LOG_SELECT("  ", "VitoHome Select", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X  Length: %u  Options: %zu", this->datapoint_.address(),
                this->datapoint_.length(), this->raw_values_.size());
  if (this->extract_byte_ >= 0) {
    ESP_LOGCONFIG(TAG, "    Extract: %u byte(s) at offset %d of a %u-byte block read", this->extract_len_,
                  this->extract_byte_, this->datapoint_.length());
  }
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
  ESP_LOGD(TAG, "%s: queued write option %zu (raw 0x%02" PRIX32 ")", this->datapoint_.name(), index, raw);
}

void VitoSelect::handle_response(const ResponseView &response) {
  // With extraction the response is the whole block read at the state
  // address; the enum field is extract_len_ bytes at extract_byte_. The
  // bound check is against the bytes actually received, so a short response
  // fail-softs instead of reading past the end.
  const uint8_t *p = response.data;
  uint8_t len = this->datapoint_.length();
  if (this->extract_byte_ >= 0) {
    const uint16_t off = static_cast<uint16_t>(this->extract_byte_);
    if (off + this->extract_len_ > response.data_length) {
      ESP_LOGW(TAG, "%s: response too short (have %u bytes, need %u)", this->datapoint_.name(), response.data_length,
               static_cast<unsigned>(off + this->extract_len_));
      return;
    }
    p += off;
    len = this->extract_len_;
  } else if (response.data_length < len) {
    ESP_LOGW(TAG, "%s: response too short (have %u bytes, need %u)", this->datapoint_.name(), response.data_length,
             len);
    return;
  }
  const uint32_t raw = static_cast<uint32_t>(read_le(p, len > 4 ? 4 : len));
  for (size_t i = 0; i < this->raw_values_.size(); i++) {
    if (this->raw_values_[i] == raw) {
      ESP_LOGD(TAG, "%s = option %zu (raw 0x%02" PRIX32 ")", this->datapoint_.name(), i, raw);
      this->publish_state(i);
      return;
    }
  }
  // ESPHome selects can only publish mapped options; an unmapped wire value
  // stays unpublished and is surfaced in the log instead.
  ESP_LOGW(TAG, "%s: device value 0x%02" PRIX32 " is not in the configured options", this->datapoint_.name(), raw);
}

void VitoSelect::handle_write_response(const ResponseView & /*response*/) {
  if (!this->read_back_) {
    this->publish_state(this->pending_index_);
  }
}

void VitoSelect::handle_error(optolink::OptolinkResult /*error*/) {
  // Keep the last option; the hub logs the specific error.
}

}  // namespace esphome::vitohome
#endif  // USE_SELECT
