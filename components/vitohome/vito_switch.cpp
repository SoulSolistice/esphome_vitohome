#include "vito_switch.h"
#ifdef USE_SWITCH

#include <cinttypes>

#include "decode.h"
#include "esphome/core/log.h"
#include "vitohome.h"

namespace esphome::vitohome {

static const char *const TAG = "vitohome.switch";

void VitoSwitch::dump_config() {
  LOG_SWITCH("  ", "Switch", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X  Length: %u  On: 0x%02" PRIX32 "  Off: 0x%02" PRIX32,
                this->datapoint_.address(), this->datapoint_.length(), this->on_value_, this->off_value_);
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

void VitoSwitch::write_state(bool state) {
  const uint32_t raw = state ? this->on_value_ : this->off_value_;
  // Bit field: on/off are FIELD values (see VitoSelect::control()).
  if (this->field_mask_ != 0 && raw > bit_field_max(this->field_mask_)) {
    ESP_LOGE(TAG, "%s: value 0x%02" PRIX32 " does not fit bit_mask 0x%02X - not written", this->datapoint_.name(), raw,
             this->field_mask_);
    return;
  }
  // See VitoSelect::control(): encode_raw_le() clamps to the staging buffer and
  // returns the effective width.
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
  this->pending_state_ = state;
  if (this->vh_parent_ == nullptr || !this->vh_parent_->request_write(this)) {
    ESP_LOGE(TAG, "%s: write could not be queued", this->datapoint_.name());
    return;
  }
  // Not optimistic: like VitoSelect, the published state changes on read-back
  // (or on the write ack when read_back is off), never on the queue action.
  ESP_LOGD(TAG, "%s: queued write %s (raw 0x%02" PRIX32 ")", this->datapoint_.name(), ONOFF(state), raw);
}

void VitoSwitch::handle_response(const ResponseView &response) {
  uint32_t raw = 0;
  if (!this->field_value_(response, &raw)) {
    ESP_LOGW(TAG, "%s: response too short (have %u bytes, need %u)", this->datapoint_.name(), response.data_length,
             this->field_end_());
    return;
  }
  for (uint16_t i = 0; i < this->on_state_count_; i++) {
    if (raw == this->on_state_values_[i]) {
      ESP_LOGD(TAG, "%s = ON (raw 0x%02" PRIX32 ")", this->datapoint_.name(), raw);
      this->publish_state(true);
      return;
    }
  }
  if (raw == this->off_value_) {
    ESP_LOGD(TAG, "%s = OFF (raw 0x%02" PRIX32 ")", this->datapoint_.name(), raw);
    this->publish_state(false);
    return;
  }
  // Same policy as VitoSelect for unmapped wire values: keep the last state
  // and surface the raw value in the log (add it to on_values if it means on).
  ESP_LOGW(TAG, "%s: device value 0x%02" PRIX32 " is neither an on_values entry nor off_value", this->datapoint_.name(),
           raw);
}

void VitoSwitch::handle_write_response(const ResponseView & /*response*/) {
  if (!this->read_back_) {
    this->publish_state(this->pending_state_);
  }
}

void VitoSwitch::handle_error(optolink::OptolinkResult /*error*/) {
  // Keeps the last value: ESPHome has no public way to report this entity type
  // as unknown. The hub's connectivity sensor shows a lost link.
}

}  // namespace esphome::vitohome
#endif  // USE_SWITCH
