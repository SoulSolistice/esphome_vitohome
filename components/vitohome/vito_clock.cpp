#include "vito_clock.h"
#ifdef VITOHOME_TIME_SYNC

#include <cinttypes>

#include "decode.h"
#include "esphome/core/log.h"
#include "vitohome.h"

namespace esphome::vitohome {

static const char *const TAG = "vitohome.clock";

VitoClock::VitoClock() {
  this->set_clock_address(CLOCK_ADDRESS_DEFAULT);

  // wants_read_back() defaults true, which is what makes the verify step free:
  // the hub's write-ACK path pushes the read-back to the head of the read lane
  // for us. Stated explicitly because the whole VERIFYING phase depends on it.
  this->read_back_ = true;
}

// The ADDRESS is configurable (see the header); the LENGTH is not. Both known
// DateTimeBCD variants -- NRF 0x088E and WPR 0x08E0 -- are 8 bytes, and 8 is
// also exactly VitoEntityBase::write_buf_, which is what lets the clock ride
// the ordinary entity write path with no growth anywhere.
void VitoClock::set_clock_address(uint16_t address) {
  this->clock_address_ = address;
  // noconv: like every other vitohome entity, this decodes and encodes the raw
  // payload itself (decode.h) rather than using an optolink library converter.
  this->set_datapoint(optolink::Datapoint("clock", address, CLOCK_LEN, optolink::noconv));
}

void VitoClock::tick(uint32_t now_ms) {
  if (this->time_source_ == nullptr)
    return;

  // A chain is already walking read -> write -> verify. Starting a second one
  // would interleave two compares against the same datapoint. The old raw-lane
  // implementation got this for free from a depth-1 queue rejecting the second
  // enqueue; on the read lane (which cannot fill) the guard has to be explicit.
  if (this->phase_ != Phase::IDLE)
    return;

  if (!this->did_boot_) {
    // Defer the first sync until the time source has a valid time at least once.
    if (!this->time_source_->now().is_valid())
      return;

    this->did_boot_ = true;
    this->next_sync_ms_ = now_ms + this->interval_ms_;

    if (!this->sync_on_boot_)
      return;
  } else {
    if (this->interval_ms_ == 0)
      return;

    if (static_cast<int32_t>(now_ms - this->next_sync_ms_) < 0)
      return;

    this->next_sync_ms_ = now_ms + this->interval_ms_;
  }

  if (!this->time_source_->now().is_valid()) {
    ESP_LOGW(TAG, "System-time sync: time source not valid yet, skipping");
    return;
  }

  if (this->vh_parent_ == nullptr)
    return;

  // Head of the read lane, ahead of pending polls: preserves the dispatch
  // priority the raw lane used to give this. See the PRIORITY note in the
  // header.
  if (!this->vh_parent_->request_priority_read(this)) {
    ESP_LOGW(TAG, "System-time sync: clock read could not be queued");
    return;
  }

  this->phase_ = Phase::READING;
}

void VitoClock::abort_(const char *why) {
  ESP_LOGW(TAG, "System-time sync: %s", why);
  this->phase_ = Phase::IDLE;
}

void VitoClock::handle_response(const ResponseView &response) {
  switch (this->phase_) {
    case Phase::READING:
      this->handle_read_(response);
      return;

    case Phase::VERIFYING:
      this->handle_verify_(response);
      return;

    case Phase::IDLE:
      // A response with no chain in flight. Not reachable through tick(), but
      // the entity is in entities_ and a future caller (refresh_all(), a manual
      // read) could route one here. Ignore it rather than comparing drift and
      // writing off the back of an unsolicited read.
      ESP_LOGD(TAG, "System-time sync: response with no sync in flight, ignoring");
      return;
  }
}

void VitoClock::handle_read_(const ResponseView &response) {
  // The compare is finished with this read either way; only a successful write
  // request re-arms the chain into VERIFYING.
  this->phase_ = Phase::IDLE;

  if (this->time_source_ == nullptr)
    return;

  const ESPTime time = this->time_source_->now();

  if (!time.is_valid()) {
    ESP_LOGW(TAG, "System-time sync: time source became invalid, skipping");
    return;
  }

  BcdDateTime device_time{};
  const bool device_time_ok = decode_datetime_bcd(response.data, response.data_length, 0, &device_time);

  bool need_write = true;

  if (device_time_ok) {
    BcdDateTime source_time{};
    source_time.year = time.year;
    source_time.month = time.month;
    source_time.day = time.day_of_month;
    source_time.hour = time.hour;
    source_time.minute = time.minute;
    source_time.second = time.second;

    const int64_t drift = civil_seconds(source_time) - civil_seconds(device_time);
    const int64_t magnitude = drift < 0 ? -drift : drift;

    if (magnitude <= static_cast<int64_t>(this->drift_threshold_s_)) {
      ESP_LOGD(TAG, "System-time sync: drift %lds within %" PRIu32 "s, no write", static_cast<long>(drift),
               this->drift_threshold_s_);
      need_write = false;
    } else {
      ESP_LOGI(TAG, "System-time sync: drift %lds exceeds %" PRIu32 "s, updating device clock",
               static_cast<long>(drift), this->drift_threshold_s_);
    }
  } else {
    ESP_LOGI(TAG, "System-time sync: device clock unreadable, setting it");
  }

  if (!need_write)
    return;

  uint8_t buffer[CLOCK_LEN];
  const uint8_t weekday = device_weekday_from_esptime(time.day_of_week);

  if (!encode_datetime_bcd(time.year, time.month, time.day_of_month, weekday, time.hour, time.minute, time.second,
                           buffer)) {
    ESP_LOGW(TAG, "System-time sync: time source out of range, skipping");
    return;
  }

  if (!this->set_write_payload_(buffer, CLOCK_LEN)) {
    ESP_LOGW(TAG, "System-time sync: clock payload rejected by the write buffer");
    return;
  }

  if (this->vh_parent_ == nullptr)
    return;

  if (!this->vh_parent_->request_write(this)) {
    ESP_LOGW(TAG, "System-time sync: clock write could not be queued");
    return;
  }

  // The chain continues in handle_write_response() once the device ACKs.
}

void VitoClock::handle_write_response(const ResponseView & /*response*/) {
  ESP_LOGI(TAG, "System-time sync: device clock set; reading back to confirm");

  // Arm the verify BEFORE returning: the hub enqueues the read-back
  // (wants_read_back()) immediately after this call returns, so the phase must
  // already be VERIFYING when that response lands.
  this->phase_ = Phase::VERIFYING;
}

void VitoClock::handle_verify_(const ResponseView &response) {
  this->phase_ = Phase::IDLE;

  BcdDateTime device_time{};

  if (decode_datetime_bcd(response.data, response.data_length, 0, &device_time)) {
    ESP_LOGI(TAG, "System-time sync: device clock now %04u-%02u-%02u %02u:%02u:%02u", device_time.year,
             device_time.month, device_time.day, device_time.hour, device_time.minute, device_time.second);
  } else {
    ESP_LOGW(TAG, "System-time sync: read-back of device clock unreadable");
  }
}

void VitoClock::handle_error(optolink::OptolinkResult error) {
  // Read error: the hub logs the specific protocol result. Reset so the next
  // tick() can start a fresh chain rather than wedging in READING/VERIFYING
  // forever.
  this->abort_(this->phase_ == Phase::VERIFYING ? "read-back of device clock failed" : "clock read failed");
  (void) error;
}

void VitoClock::handle_write_error(optolink::OptolinkResult error) {
  this->abort_("device clock write failed");
  (void) error;
}

void VitoClock::dump_config() {
  // Called explicitly by VitoHomeComponent::dump_config(), unlike every other
  // entity: those are registered ESPHome components that core's dump_config()
  // fan-out reaches on its own, which is exactly why the hub has no loop over
  // entities_. VitoClock is hub-owned and not a component, so nothing would
  // print it otherwise.
  ESP_LOGCONFIG(TAG, "vitohome clock (system-time sync):");
  ESP_LOGCONFIG(TAG, "  Datapoint: 0x%04X, %u bytes", this->clock_address_, CLOCK_LEN);

  if (this->interval_ms_ == 0) {
    ESP_LOGCONFIG(TAG, "  Periodic sync: OFF");
  } else {
    ESP_LOGCONFIG(TAG, "  Periodic sync: every %" PRIu32 " ms", this->interval_ms_);
  }

  ESP_LOGCONFIG(TAG, "  Drift threshold: %" PRIu32 " s", this->drift_threshold_s_);
  ESP_LOGCONFIG(TAG, "  Sync on boot: %s", YESNO(this->sync_on_boot_));
}

}  // namespace esphome::vitohome

#endif  // VITOHOME_TIME_SYNC
