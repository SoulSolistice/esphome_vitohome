#include "vito_text.h"

#include "decode.h"
#include "esphome/core/log.h"
#include "vitohome.h"

namespace esphome {
namespace vitohome {

static const char* const TAG = "vitohome.text";

// A per-day program is 8 bytes; the canonical string is at most
// "HH:MM-HH:MM" x4 + 3 spaces = 47 chars, +NUL.
static constexpr uint8_t SCHALTZEITEN_LEN = 8;

void VitoText::dump_config() {
  LOG_TEXT("  ", "VitoHome Text", this);
  ESP_LOGCONFIG(TAG, "    Type: schaltzeiten  Address: 0x%04X  Length: %u  read_back: %s", this->datapoint_.address(),
                this->datapoint_.length(), this->read_back_ ? "yes" : "no");
}

void VitoText::control(const std::string& value) {
  uint8_t buf[SCHALTZEITEN_LEN];
  if (!encode_schaltzeiten_day(value.c_str(), buf)) {
    // Unparseable program (bad time, too many pairs, ...). Refuse to transmit
    // rather than write a partial/wrong schedule; the device keeps its value.
    ESP_LOGE(TAG, "%s: '%s' is not a valid switching-time program — not written", this->datapoint_.name(),
             value.c_str());
    return;
  }
  if (!this->set_write_payload_(buf, SCHALTZEITEN_LEN)) {
    ESP_LOGE(TAG, "%s: failed to stage write payload", this->datapoint_.name());
    return;
  }
  this->pending_value_ = value;
  if (this->vh_parent_ == nullptr || !this->vh_parent_->request_write(this)) {
    ESP_LOGE(TAG, "%s: write could not be queued", this->datapoint_.name());
    return;
  }
  ESP_LOGD(TAG, "%s: queued write '%s'", this->datapoint_.name(), value.c_str());
}

void VitoText::handle_response(const ResponseView& response) {
  char out[64];
  if (decode_schaltzeiten_day(response.data, response.data_length, out, sizeof(out)) < 0) {
    ESP_LOGW(TAG, "%s: response too short (have %u bytes, need %u)", this->datapoint_.name(), response.data_length,
             this->datapoint_.length());
    return;
  }
  ESP_LOGD(TAG, "%s = '%s'", this->datapoint_.name(), out);
  this->publish_state(std::string(out));
}

void VitoText::handle_write_response(const ResponseView& /*response*/) {
  if (!this->read_back_) {
    // No read-back requested: publish optimistically on the device ACK. With
    // read_back (default) the hub immediately re-reads this address and
    // handle_response() publishes the device's own canonical view (which may
    // differ from the input after 10-minute truncation).
    this->publish_state(this->pending_value_);
  }
}

void VitoText::handle_error(optolink::OptolinkResult /*error*/) {
  // Keep the last published value; a transient read/write error should not
  // blank the entity. The hub already logs the protocol-level error.
}

}  // namespace vitohome
}  // namespace esphome
