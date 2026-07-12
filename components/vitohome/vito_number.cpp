#include "vito_number.h"

#include <cmath>

#include "decode.h"
#include "esphome/core/log.h"
#include "vitohome.h"

namespace esphome::vitohome {

static const char *const TAG = "vitohome.number";

void VitoNumber::dump_config() {
  LOG_NUMBER("  ", "VitoHome Number", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X  Length: %u  scale: %g  signed: %s  read_back: %s",
                this->datapoint_.address(), this->datapoint_.length(), this->scale_, this->signed_ ? "yes" : "no",
                this->read_back_ ? "yes" : "no");
  if (this->extract_byte_ >= 0) {
    ESP_LOGCONFIG(TAG, "    Extract: %u byte(s) at offset %d of a %u-byte block read", this->extract_len_,
                  this->extract_byte_, this->datapoint_.length());
  }
}

void VitoNumber::control(float value) {
  uint8_t buf[4];
  // The wire width of the WRITE: the write datapoint's length (the field
  // width under block extraction, or with a read/write address split; falls
  // back to datapoint_ for the single-address case).
  const uint8_t len = this->get_write_datapoint().length();
  if (!encode_scaled(static_cast<double>(value), this->scale_, this->signed_, len, buf)) {
    // Out-of-range for the wire representation. The Python schema already
    // cross-checks min/max against the encodable range, so reaching this
    // means a runtime caller bypassed the traits — refuse to transmit.
    ESP_LOGE(TAG, "%s: value %.3f not encodable (scale %g, %s, %u bytes) — not written", this->datapoint_.name(), value,
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
  double value = NAN;
  bool ok;
  if (this->extract_byte_ >= 0) {
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
  this->consecutive_read_errors_ = 0;
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
  // Read errors only (writes go to handle_write_error, a keep-state no-op):
  // blank the entity only after a streak, as in VitoSensor.
  if (this->consecutive_read_errors_ < NAN_AFTER_CONSECUTIVE_READ_ERRORS) {
    this->consecutive_read_errors_++;
  }
  if (this->consecutive_read_errors_ == NAN_AFTER_CONSECUTIVE_READ_ERRORS) {
    this->publish_state(NAN);
  }
}

}  // namespace esphome::vitohome
