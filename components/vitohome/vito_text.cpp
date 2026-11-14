#include "vito_text.h"
#ifdef USE_TEXT

#include "decode.h"
#include "esphome/core/log.h"
#include "vitohome.h"

namespace esphome::vitohome {

static const char *const TAG = "vitohome.text";

// A per-day program is 8 bytes; the canonical string is at most
// "HH:MM-HH:MM" x4 + 3 spaces = 47 chars, +NUL.
static constexpr uint8_t SCHALTZEITEN_LEN = 8;

void VitoText::dump_config() {
  LOG_TEXT("  ", "Text", this);
  ESP_LOGCONFIG(TAG, "    Type: schaltzeiten  Address: 0x%04X  Length: %u  read_back: %s", this->datapoint_.address(),
                this->datapoint_.length(), this->write_path_.read_back ? "yes" : "no");
}

void VitoText::control(const std::string &value) {
  uint8_t buf[SCHALTZEITEN_LEN];
  bool rounded = false;
  if (!encode_schaltzeiten_day(value.c_str(), buf, &rounded)) {
    // Nothing is sent: a partial program never reaches the device.
    ESP_LOGE(TAG, "%s: '%s' is not a valid switching-time program - not written", this->datapoint_.name(),
             value.c_str());
    return;
  }
  // What is written, in the canonical form the read-back will show.
  char canon[48];
  decode_schaltzeiten_day(buf, SCHALTZEITEN_LEN, canon, sizeof(canon));
  if (rounded) {
    ESP_LOGW(TAG, "%s: switch points are stored in 10-minute steps; '%s' is written as '%s'", this->datapoint_.name(),
             value.c_str(), canon);
  }
  if (!this->set_write_payload_(buf, SCHALTZEITEN_LEN)) {
    ESP_LOGE(TAG, "%s: failed to stage write payload", this->datapoint_.name());
    return;
  }
  this->pending_value_ = canon;
  if (this->vh_parent_ == nullptr || !this->vh_parent_->request_write(this)) {
    ESP_LOGE(TAG, "%s: write could not be queued", this->datapoint_.name());
    return;
  }
  ESP_LOGD(TAG, "%s: queued write '%s'", this->datapoint_.name(), canon);
}

void VitoText::handle_response(const ResponseView &response) {
  char out[64];
  bool malformed = false;
  if (decode_schaltzeiten_day(response.data, response.data_length, out, sizeof(out), &malformed) < 0) {
    ESP_LOGW(TAG, "%s: response too short (have %u bytes, need %u)", this->datapoint_.name(), response.data_length,
             this->datapoint_.length());
    return;
  }
  if (malformed) {
    ESP_LOGW(TAG,
             "%s: '%s' holds a switch point that is no time (shown as !XX); enter the program in full to replace it",
             this->datapoint_.name(), out);
  } else {
    ESP_LOGD(TAG, "%s = '%s'", this->datapoint_.name(), out);
  }
  this->publish_state(out);  // const char *: reuses the state string, no temporary
}

void VitoText::handle_write_response(const ResponseView & /*response*/) {
  // With read_back (default) the hub re-reads the address and handle_response()
  // publishes the device's view; without it, the acknowledged program.
  if (!this->write_path_.read_back)
    this->publish_state(this->pending_value_);
}

void VitoText::handle_error(optolink::OptolinkResult /*error*/) {
  // Keeps the last value: ESPHome has no public way to report this entity type
  // as unknown. The hub's connectivity sensor shows a lost link.
}

}  // namespace esphome::vitohome
#endif  // USE_TEXT
