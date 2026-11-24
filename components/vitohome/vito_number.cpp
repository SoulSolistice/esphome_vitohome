#include "vito_number.h"
#ifdef USE_NUMBER

#include <algorithm>
#include <cmath>

#include "decode.h"
#include "esphome/core/log.h"
#include "vitohome.h"

namespace esphome::vitohome {

static const char *const TAG = "vitohome.number";

void VitoNumber::dump_config() {
  LOG_NUMBER("  ", "Number", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X  Length: %u  scale: %g  offset: %g  signed: %s  read_back: %s",
                this->datapoint_.address(), this->datapoint_.length(), this->scale_, this->value_offset_,
                this->signed_ ? "yes" : "no", this->write_path_.read_back ? "yes" : "no");
  if (this->extract_byte_ >= 0) {
    ESP_LOGCONFIG(TAG, "    Extract: %u byte(s) at offset %d of a %u-byte block read", this->extract_len_,
                  this->extract_byte_, this->datapoint_.length());
  }
  if (this->field_mask_ != 0) {
    ESP_LOGCONFIG(TAG, "    Bit mask: 0x%02X (writes are read-modify-write)", this->field_mask_);
  }
  if (this->write_path_.block_width != 0) {
    ESP_LOGCONFIG(TAG, "    Write: whole %u-byte block, read-modify-write of %u byte(s) at offset %u",
                  this->get_write_datapoint().length(), this->write_path_.block_width, this->write_path_.block_offset);
  }
}

void VitoNumber::control(float value) {
  uint8_t buf[4];
  // The width staged for the WRITE: the field in block mode (the hub merges it
  // into the block it reads first), else the write datapoint's length (the
  // field width under the interior shape, or with a read/write address split;
  // falls back to datapoint_ for the single-address case).
  const uint8_t len = this->staged_width();
  const double raw_value = static_cast<double>(value) - this->value_offset_;
  if (this->field_mask_ != 0) {
    // Bit field: stage the FIELD value, unsigned, in one byte. The hub merges
    // it into the byte it reads back first (read-modify-write), so the bits
    // outside the mask are never touched here. number.py checks min/max
    // against the mask width; this is the runtime twin of that check.
    if (!encode_scaled(raw_value, this->scale_, false, 1, buf) || buf[0] > bit_field_max(this->field_mask_)) {
      ESP_LOGE(TAG, "%s: value %.3f does not fit bit_mask 0x%02X (scale %g) - not written", this->datapoint_.name(),
               value, this->field_mask_, this->scale_);
      return;
    }
    if (!this->set_write_payload_(buf, 1)) {
      ESP_LOGE(TAG, "%s: failed to stage write payload", this->datapoint_.name());
      return;
    }
    this->pending_value_ = value;
    if (this->vh_parent_ == nullptr || !this->vh_parent_->request_write(this)) {
      ESP_LOGE(TAG, "%s: write could not be queued", this->datapoint_.name());
      return;
    }
    ESP_LOGD(TAG, "%s: queued write %.3f (field 0x%02X, mask 0x%02X)", this->datapoint_.name(), value, buf[0],
             this->field_mask_);
    return;
  }
  if (!encode_scaled(raw_value, this->scale_, this->signed_, len, buf)) {
    // Out-of-range for the wire representation. The Python schema already
    // cross-checks min/max against the encodable range, so reaching this
    // means a runtime caller bypassed the traits — refuse to transmit.
    ESP_LOGE(TAG, "%s: value %.3f not encodable (scale %g, %s, %u bytes) - not written", this->datapoint_.name(), value,
             this->scale_, this->signed_ ? "signed" : "unsigned", len);
    return;
  }
  if (!this->set_write_payload_(buf, len)) {
    ESP_LOGE(TAG, "%s: failed to stage write payload", this->datapoint_.name());
    return;
  }
  this->pending_value_ = value;
  if (this->vh_parent_ == nullptr || !this->vh_parent_->request_write(this)) {
    ESP_LOGE(TAG, "%s: write could not be queued", this->datapoint_.name());
    return;
  }
  ESP_LOGD(TAG, "%s: queued write %.3f", this->datapoint_.name(), value);
}

void VitoNumber::handle_response(const ResponseView &response) {
  // The read-back after a write, and the poll that reflects panel changes.
  if (response.data == nullptr) {
    ESP_LOGW(TAG, "%s: null response payload", this->datapoint_.name());
    return;
  }
  const uint8_t width = this->field_width_();
  double value = NAN;
  bool ok;
  if (this->field_mask_ != 0) {
    // Unsigned, as in VitoSensor (see control()).
    uint32_t field = 0;
    ok = this->field_value_(response, &field);
    const uint8_t bits = static_cast<uint8_t>(field);
    ok = ok && decode_scaled(&bits, 1, 1, false, this->scale_, &value);
  } else {
    const uint8_t *field = nullptr;
    ok =
        this->field_bytes_(response, &field) && decode_scaled(field, width, width, this->signed_, this->scale_, &value);
  }
  if (!ok) {
    ESP_LOGW(TAG, "%s: response too short (have %u bytes, need %u)", this->datapoint_.name(), response.data_length,
             this->field_end_());
    this->handle_error(optolink::OptolinkResult::LENGTH);
    return;
  }
  const float out = static_cast<float>(value + this->value_offset_);
  if (std::isnan(out) || std::isinf(out)) {
    ESP_LOGW(TAG, "%s: decoded non-finite value, skipping", this->datapoint_.name());
    this->handle_error(optolink::OptolinkResult::LENGTH);
    return;
  }
  // A unit answers an address it does not serve with 0xFF in every byte (GWG
  // `be` on a BES). Outside the entity's range that is no value: unknown,
  // not 255. A signed field keeps 0xFF when -1 is in range.
  const uint8_t *bytes = nullptr;
  if (this->field_bytes_(response, &bytes) && std::all_of(bytes, bytes + width, [](uint8_t b) { return b == 0xFF; }) &&
      (out < this->traits.get_min_value() || out > this->traits.get_max_value())) {
    ESP_LOGD(TAG, "%s: 0xFF, outside %.3f..%.3f: unknown", this->datapoint_.name(), this->traits.get_min_value(),
             this->traits.get_max_value());
    this->clear_read_failures_();
    this->publish_state(NAN);
    return;
  }
  ESP_LOGD(TAG, "%s = %.3f", this->datapoint_.name(), out);
  this->clear_read_failures_();
  this->publish_state(out);
}

void VitoNumber::handle_write_response(const ResponseView & /*response*/) {
  if (!this->write_path_.read_back) {
    // No read-back requested: publish optimistically on the device ACK.
    this->publish_state(this->pending_value_);
  }
  // With read_back (default) the hub immediately enqueues a read of this
  // address; handle_response() then publishes the device's own view.
}

void VitoNumber::handle_error(optolink::OptolinkResult /*error*/) {
  // Reads only; a failed write goes to handle_write_error() and keeps the state.
  if (this->count_read_failure_())
    this->publish_state(NAN);
}

}  // namespace esphome::vitohome
#endif  // USE_NUMBER
