#include "vito_number.h"
#ifdef USE_NUMBER

#include <cmath>

#include "decode.h"
#include "esphome/core/log.h"
#include "vitohome.h"

namespace esphome::vitohome {

static const char *const TAG = "vitohome.number";

void VitoNumber::dump_config() {
  LOG_NUMBER("  ", "Number", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X  Length: %u  scale: %g  signed: %s  read_back: %s",
                this->datapoint_.address(), this->datapoint_.length(), this->scale_, this->signed_ ? "yes" : "no",
                this->read_back_ ? "yes" : "no");
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

void VitoNumber::control(float value) {
  uint8_t buf[4];
  // The width staged for the WRITE: the field in block mode (the hub merges it
  // into the block it reads first), else the write datapoint's length (the
  // field width under the interior shape, or with a read/write address split;
  // falls back to datapoint_ for the single-address case).
  const uint8_t len = this->staged_width();
  if (this->field_mask_ != 0) {
    // Bit field: stage the FIELD value, unsigned, in one byte. The hub merges
    // it into the byte it reads back first (read-modify-write), so the bits
    // outside the mask are never touched here. number.py checks min/max
    // against the mask width; this is the runtime twin of that check.
    if (!encode_scaled(static_cast<double>(value), this->scale_, false, 1, buf) ||
        buf[0] > bit_field_max(this->field_mask_)) {
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
  if (!encode_scaled(static_cast<double>(value), this->scale_, this->signed_, len, buf)) {
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
  // Same decode path as the sensor platform: read-back after a write, and
  // the periodic poll that reflects panel-side changes. With extraction the
  // response is the whole block read at the state address; the numeric field
  // is extract_len_ bytes at extract_byte_ (bound-checked against the bytes
  // actually received, like VitoSensor).
  if (response.data == nullptr) {
    ESP_LOGW(TAG, "%s: null response payload", this->datapoint_.name());
    return;
  }
  double value = NAN;
  bool ok;
  if (this->field_mask_ != 0) {
    // Bit field: the same extraction as VitoSensor -- one byte, masked and
    // shifted to the field, then scaled (unsigned; see control()).
    const uint16_t off = this->extract_byte_ >= 0 ? static_cast<uint16_t>(this->extract_byte_) : 0;
    uint8_t field = 0;
    ok = off < response.data_length && extract_bit_field(response.data[off], this->field_mask_, &field) &&
         decode_scaled(&field, 1, 1, false, this->scale_, &value);
  } else if (this->extract_byte_ >= 0) {
    const uint16_t off = static_cast<uint16_t>(this->extract_byte_);
    ok =
        off + this->extract_len_ <= response.data_length &&
        decode_scaled(response.data + off, this->extract_len_, this->extract_len_, this->signed_, this->scale_, &value);
  } else {
    ok = decode_scaled(response.data, response.data_length, this->datapoint_.length(), this->signed_, this->scale_,
                       &value);
  }
  if (!ok) {
    ESP_LOGW(TAG, "%s: response too short (have %u bytes, need %u)", this->datapoint_.name(), response.data_length,
             this->extract_byte_ >= 0 ? static_cast<unsigned>(this->extract_byte_ + this->extract_len_)
                                      : this->datapoint_.length());
    // A decode failure is a failed read (same rule as VitoSensor): advance
    // the streak so a persistently short/garbage response eventually blanks
    // the entity instead of pinning the last good value forever.
    this->handle_error(optolink::OptolinkResult::LENGTH);
    return;
  }
  const float out = static_cast<float>(value);
  if (std::isnan(out) || std::isinf(out)) {
    ESP_LOGW(TAG, "%s: decoded non-finite value, skipping", this->datapoint_.name());
    this->handle_error(optolink::OptolinkResult::LENGTH);
    return;
  }
  ESP_LOGD(TAG, "%s = %.3f", this->datapoint_.name(), out);
  this->clear_read_failures_();
  this->publish_state(out);
}

void VitoNumber::handle_write_response(const ResponseView & /*response*/) {
  if (!this->read_back_) {
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
