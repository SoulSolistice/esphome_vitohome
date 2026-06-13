#include "vito_number.h"

#include <cmath>

#include "decode.h"
#include "esphome/core/log.h"
#include "vitohome.h"

namespace esphome {
namespace vitohome {

static const char *const TAG = "vitohome.number";

void VitoNumber::dump_config() {
  LOG_NUMBER("  ", "VitoHome Number", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X  Length: %u  scale: %g  signed: %s  read_back: %s",
                this->datapoint_.address(), this->datapoint_.length(), this->scale_, this->signed_ ? "yes" : "no",
                this->read_back_ ? "yes" : "no");
}

void VitoNumber::control(float value) {
  uint8_t buf[4];
  const uint8_t len = this->datapoint_.length();
  if (!encode_scaled(static_cast<double>(value), this->scale_, this->signed_, len, buf)) {
    // Out-of-range for the wire representation. The Python schema already
    // cross-checks min/max against the encodable range, so reaching this
    // means a runtime caller bypassed the traits — refuse to transmit.
    ESP_LOGE(TAG, "%s: value %.3f not encodable (scale %g, %s, %u bytes) — not written", this->datapoint_.name(),
             value, this->scale_, this->signed_ ? "signed" : "unsigned", len);
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

void VitoNumber::handle_response(const VitoWiFi::PacketVS2 &response) {
  // Same decode path as the sensor platform: read-back after a write, and
  // the periodic poll that reflects panel-side changes.
  double value = NAN;
  if (!decode_scaled(response.data(), response.dataLength(), this->datapoint_.length(), this->signed_, this->scale_,
                     &value)) {
    ESP_LOGW(TAG, "%s: response too short (have %u bytes, need %u)", this->datapoint_.name(), response.dataLength(),
             this->datapoint_.length());
    return;
  }
  const float out = static_cast<float>(value);
  if (std::isnan(out) || std::isinf(out)) {
    ESP_LOGW(TAG, "%s: decoded non-finite value, skipping", this->datapoint_.name());
    return;
  }
  ESP_LOGD(TAG, "%s = %.3f", this->datapoint_.name(), out);
  this->publish_state(out);
}

void VitoNumber::handle_write_response(const VitoWiFi::PacketVS2 & /*response*/) {
  if (!this->read_back_) {
    // No read-back requested: publish optimistically on the device ACK.
    this->publish_state(this->pending_value_);
  }
  // With read_back (default) the hub immediately enqueues a read of this
  // address; handle_response() then publishes the device's own view.
}

void VitoNumber::handle_error(VitoWiFi::OptolinkResult /*error*/) { this->publish_state(NAN); }

}  // namespace vitohome
}  // namespace esphome
