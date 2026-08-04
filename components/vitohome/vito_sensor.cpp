#include "vito_sensor.h"
#ifdef USE_SENSOR

#include <cmath>

#include "decode.h"
#include "esphome/core/log.h"

namespace esphome::vitohome {

static const char *const TAG = "vitohome.sensor";

void VitoSensor::dump_config() {
  LOG_SENSOR("  ", "Sensor", this);
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X  Length: %u  scale: %g  signed: %s", this->datapoint_.address(),
                this->datapoint_.length(), this->scale_, this->signed_ ? "yes" : "no");
  if (this->extract_byte_ >= 0) {
    ESP_LOGCONFIG(TAG, "    Extract: %u byte(s) at offset %d of a %u-byte block read", this->extract_len_,
                  this->extract_byte_, this->datapoint_.length());
  }
}

void VitoSensor::handle_response(const ResponseView &response) {
  // Component decode path: vitohome decodes the raw payload itself in decode.h
  // (uint64_t read, double math, float32 only at publish). Upstream's converter
  // layer — float math and the tagless VariantValue union — is fully removed
  // from the vendored engine (THIRD_PARTY.md items 13/15); see
  // docs/design_notes.md SS1 for why this path exists.
  // A null payload with a NON-ZERO length is a documented engine state, not a
  // hypothetical: PacketVS2::data() returns nullptr for a WRITE frame while
  // dataLength() stays non-zero, and vs2.cpp::_tryOnResponse states the
  // obligation outright ("the caller must guard on data() before reading").
  // The else-branch below is safe without this (decode_scaled null-checks), but
  // the extraction branch forms `data + off` before any check.
  if (response.data == nullptr) {
    ESP_LOGW(TAG, "%s: null response payload", this->datapoint_.name());
    return;
  }
  const uint8_t *data = response.data;
  const uint8_t have = response.data_length;
  double value = NAN;
  bool ok;
  if (this->extract_byte_ >= 0) {
    // Fetch the whole block (datapoint length), then scale/sign the field of
    // extract_len_ bytes at the offset. The bound check is against the bytes
    // actually received, so a short response fail-softs instead of reading
    // past the end. Little-endian only: every extracted field in the Vitosoft
    // data is LE, and sensor.py rejects a big-endian converter combined with
    // byte_offset, so a big-endian extracted field cannot reach this path.
    const uint16_t off = static_cast<uint16_t>(this->extract_byte_);
    ok = off + this->extract_len_ <= have &&
         decode_scaled(data + off, this->extract_len_, this->extract_len_, this->signed_, this->scale_, &value);
  } else {
    ok = this->big_endian_
             ? decode_scaled_be(data, have, this->datapoint_.length(), this->signed_, this->scale_, &value)
             : decode_scaled(data, have, this->datapoint_.length(), this->signed_, this->scale_, &value);
  }
  if (!ok) {
    ESP_LOGW(TAG, "%s: response too short (have %u bytes, need %u)", this->datapoint_.name(), have,
             this->extract_byte_ >= 0 ? static_cast<unsigned>(this->extract_byte_ + this->extract_len_)
                                      : this->datapoint_.length());
    // A decode failure is a failed read: advance the streak so a persistently
    // short/garbage response eventually blanks the entity, rather than pinning
    // the last good value forever.
    this->note_read_failure_();
    return;
  }

  const float out = static_cast<float>(value);
  if (std::isnan(out) || std::isinf(out)) {
    ESP_LOGW(TAG, "%s: decoded non-finite value, skipping", this->datapoint_.name());
    this->note_read_failure_();
    return;
  }
  ESP_LOGD(TAG, "%s = %.3f", this->datapoint_.name(), out);
  this->consecutive_read_errors_ = 0;
  this->publish_state(out);
}

void VitoSensor::note_read_failure_() {
  // Shared streak logic for both a protocol error (handle_error) and a decode
  // failure (short or non-finite payload): blank the entity in HA only after a
  // run of failures, so a single glitch does not flap the state. A successful
  // publish resets the streak.
  if (this->consecutive_read_errors_ < NAN_AFTER_CONSECUTIVE_READ_ERRORS) {
    this->consecutive_read_errors_++;
  }
  if (this->consecutive_read_errors_ == NAN_AFTER_CONSECUTIVE_READ_ERRORS) {
    this->publish_state(NAN);
  }
}

void VitoSensor::handle_error(optolink::OptolinkResult /*error*/) {
  // Mark the entity unavailable in HA -- but only after a streak of failed
  // reads. The component logs the specific error code; we just signal
  // "no data" here once the streak crosses the threshold.
  this->note_read_failure_();
}

}  // namespace esphome::vitohome
#endif  // USE_SENSOR
