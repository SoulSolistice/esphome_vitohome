#include "vito_select.h"
#ifdef USE_SELECT

#include <cinttypes>

#include "decode.h"
#include "esphome/core/log.h"
#include "vitohome.h"

namespace esphome::vitohome {

static const char *const TAG = "vitohome.select";

void VitoSelect::dump_config() {
  LOG_SELECT("  ", "Select", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X  Length: %u  Options: %u", this->datapoint_.address(),
                this->datapoint_.length(), this->raw_value_count_);
  if (this->extract_byte_ >= 0) {
    ESP_LOGCONFIG(TAG, "    Extract: %u byte(s) at offset %d of a %u-byte block read", this->extract_len_,
                  this->extract_byte_, this->datapoint_.length());
  }
  if (this->field_mask_ != 0) {
    ESP_LOGCONFIG(TAG, "    Bit mask: 0x%02X (writes are read-modify-write)", this->field_mask_);
  }
  if (this->block_field_width_ != 0) {
    ESP_LOGCONFIG(TAG, "    Write: whole %u-byte block, read-modify-write of %u byte(s) at offset %u",
                  this->get_write_datapoint().length(), this->block_field_width_, this->block_field_offset_);
  }
}

void VitoSelect::control(size_t index) {
  if (index >= this->raw_value_count_) {
    ESP_LOGE(TAG, "%s: index %zu out of range", this->datapoint_.name(), index);
    return;
  }
  const uint32_t raw = this->raw_values_[index];
  // Bit field: the option value is the FIELD value, staged as one byte; the hub
  // merges it into the byte it reads first (read-modify-write). select.py
  // checks every option against the mask width; this is the runtime twin.
  if (this->field_mask_ != 0 && raw > bit_field_max(this->field_mask_)) {
    ESP_LOGE(TAG, "%s: option value 0x%02" PRIX32 " does not fit bit_mask 0x%02X - not written",
             this->datapoint_.name(), raw, this->field_mask_);
    return;
  }
  // encode_raw_le() clamps the write length to the staging buffer and returns
  // the effective width, so an over-long write-datapoint length can't make
  // set_write_payload_() copy past buf. Shared with VitoSwitch::write_state().
  uint8_t buf[4];
  const uint8_t len = encode_raw_le(raw, this->staged_width(), buf, sizeof(buf));
  if (len == 0) {
    ESP_LOGE(TAG, "%s: zero-length write datapoint", this->datapoint_.name());
    return;
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
  uint32_t raw = 0;
  if (!this->field_value_(response, &raw)) {
    ESP_LOGW(TAG, "%s: response too short (have %u bytes, need %u)", this->datapoint_.name(), response.data_length,
             this->field_end_());
    return;
  }
  for (uint16_t i = 0; i < this->raw_value_count_; i++) {
    if (this->raw_values_[i] == raw) {
      ESP_LOGD(TAG, "%s = option %u (raw 0x%02" PRIX32 ")", this->datapoint_.name(), i, raw);
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
  // Keeps the last value: ESPHome has no public way to report this entity type
  // as unknown. The hub's connectivity sensor shows a lost link.
}

}  // namespace esphome::vitohome
#endif  // USE_SELECT
