#include "vitohome.h"

#include <cinttypes>
#include <cstdio>

#include "decode.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#ifdef VITOHOME_TIME_SYNC
#include "esphome/components/time/real_time_clock.h"
#endif

namespace esphome {
namespace vitohome {

static const char* const TAG = "vitohome";

// Known device families (Identification at 0xF8/0xF9). Deliberately small:
// it covers the units this project has seen on the wire; everything else is
// reported as raw hex, and the catalogue tooling (scripts/gen_catalog.py)
// does the authoritative matching against the Vitosoft data.
static const char* ident_family_name(uint16_t ident) {
  switch (ident) {
    case 0x20CB:
      return "VScotHO1";
    case 0x2098:
      return "V200KW2";
    case 0x2094:
      return "V200KW1";
    case 0x2053:
      return "GWG_VBEM";
    default:
      return nullptr;
  }
}

void VitoHomeComponent::setup() {
  this->validate_uart_();
  if (this->is_failed()) return;

  // The adapter wraps the build-time-selected engine and deduces the interface
  // type, wrapping &iface_ in a GenericInterface internally.
  this->vito_ = std::make_unique<ProtocolAdapter>(&this->iface_);

  // The adapter normalises each protocol's callback shape to a ResponseView, so
  // the hub registers one uniform handler regardless of P300/KW/GWG.
  this->vito_->on_response([this](const ResponseView& response, const optolink::Datapoint& request) {
    this->on_response_(response, request);
  });
  this->vito_->on_error(
      [this](optolink::OptolinkResult error, const optolink::Datapoint& request) { this->on_error_(error, request); });

  if (!this->vito_->begin()) {
    ESP_LOGE(TAG, "optolink engine begin() failed");
    this->mark_failed();
    return;
  }

  // Require the configured protocol to establish a link within the start-up
  // window (max of the floor and 3x the hub interval); loop() marks the
  // component failed otherwise.
  uint32_t verify_window = 3u * this->get_update_interval();
  if (verify_window < PROTOCOL_VERIFY_MIN_MS) verify_window = PROTOCOL_VERIFY_MIN_MS;
  this->protocol_verify_pending_ = true;
  this->protocol_verify_deadline_ms_ = millis() + verify_window;

  // Per-entity intervals are scheduled at hub-tick granularity, so anything
  // shorter than the hub interval silently degrades to the hub interval.
  // Surface that at setup instead of letting the user chase phantom lag.
  const uint32_t hub_interval = this->get_update_interval();
  for (auto* e : this->entities_) {
    if (e->poll_interval() != 0 && e->poll_interval() < hub_interval) {
      ESP_LOGW(TAG,
               "%s '%s': update_interval %" PRIu32 " ms is shorter than the hub's %" PRIu32
               " ms; effective rate is the hub interval",
               e->entity_kind(), e->get_datapoint().name(), e->poll_interval(), hub_interval);
    }
  }

  if (this->identify_device_) {
    this->ident_start_();
  }

  ESP_LOGI(TAG, "VitoHome ready, %zu entities registered", this->entities_.size());
}

void VitoHomeComponent::validate_uart_() {
  // The Optolink requires 4800 8E2. Fail loudly here rather than spend an
  // hour debugging silent bus errors.
  auto* bus = this->parent_;
  bool ok = true;
  if (bus->get_baud_rate() != 4800) {
    ESP_LOGE(TAG, "UART baud_rate must be 4800, got %u", bus->get_baud_rate());
    ok = false;
  }
  if (bus->get_data_bits() != 8) {
    ESP_LOGE(TAG, "UART data_bits must be 8, got %u", bus->get_data_bits());
    ok = false;
  }
  if (bus->get_stop_bits() != 2) {
    ESP_LOGE(TAG, "UART stop_bits must be 2, got %u", bus->get_stop_bits());
    ok = false;
  }
  if (bus->get_parity() != uart::UART_CONFIG_PARITY_EVEN) {
    ESP_LOGE(TAG, "UART parity must be EVEN (8E2), got %d", static_cast<int>(bus->get_parity()));
    ok = false;
  }
  if (!ok) {
    ESP_LOGE(TAG, "Optolink requires 4800 8E2; fix the uart: block.");
    this->mark_failed();
  }
}

void VitoHomeComponent::loop() {
  if (this->vito_ == nullptr) return;

  this->vito_->loop();

  // Start-up protocol verification: confirm the configured protocol actually
  // established a link, or fail the component with a clear message.
  if (this->protocol_verify_pending_) {
    if (this->vito_->established()) {
      this->protocol_verify_pending_ = false;
      ESP_LOGI(TAG, "%s link established", ProtocolAdapter::protocol_name());
    } else if (millis() >= this->protocol_verify_deadline_ms_) {
      this->protocol_verify_pending_ = false;
      ESP_LOGE(TAG, "%s link not established; check wiring and that the device speaks this protocol",
               ProtocolAdapter::protocol_name());
      this->mark_failed();
    }
  }

  // Watchdog: if a request has been in flight too long, surface that and
  // free the slot.
  if (this->in_flight_ != nullptr || this->ident_in_flight_ || this->raw_in_flight_) {
    uint32_t now = millis();
    if (now - this->in_flight_started_ms_ > IN_FLIGHT_WATCHDOG_MS) {
      if (this->ident_in_flight_) {
        ESP_LOGW(TAG, "Identification read exceeded watchdog (%" PRIu32 " ms)", IN_FLIGHT_WATCHDOG_MS);
        this->ident_in_flight_ = false;
        this->ident_handle_error_();
      } else if (this->raw_in_flight_) {
        ESP_LOGW(TAG, "Raw %s 0x%04X exceeded watchdog (%" PRIu32 " ms)", this->raw_is_write_ ? "write" : "read",
                 this->raw_dp_.address(), IN_FLIGHT_WATCHDOG_MS);
        this->raw_in_flight_ = false;
        this->raw_handle_error_(optolink::OptolinkResult::TIMEOUT);
      } else {
        ESP_LOGW(TAG, "In-flight %s to %s exceeded watchdog (%" PRIu32 " ms). Clearing.",
                 this->in_flight_op_ == OpType::WRITE ? "write" : "read", this->in_flight_->get_datapoint().name(),
                 IN_FLIGHT_WATCHDOG_MS);
        if (this->in_flight_op_ == OpType::READ) {
          this->in_flight_->read_queued_ = false;
          this->in_flight_->handle_error(optolink::OptolinkResult::TIMEOUT);
        } else {
          this->in_flight_->write_in_flight_ = false;
          this->in_flight_->handle_write_error(optolink::OptolinkResult::TIMEOUT);
        }
        this->in_flight_ = nullptr;
        this->in_flight_op_ = OpType::NONE;
      }
    }
  }

  // Dispatch the next queued request if the bus is idle.
  this->dispatch_next_();
}

void VitoHomeComponent::dispatch_raw_front_() {
  const RawOp& op = this->raw_queue_.front();
  this->raw_dp_ =
      optolink::Datapoint(op.purpose == RawPurpose::SCAN ? "scan" : "clock", op.address, op.length, optolink::noconv);
  this->raw_is_write_ = op.is_write;
  this->raw_purpose_ = op.purpose;
  bool dispatched;
  if (op.is_write) {
    this->raw_write_buf_ = op.bytes;
    dispatched = this->vito_->write(this->raw_dp_, this->raw_write_buf_.data(),
                                    static_cast<uint8_t>(this->raw_write_buf_.size()));
  } else {
    this->raw_write_buf_.clear();
    dispatched = this->vito_->read(this->raw_dp_);
  }
  if (dispatched) {
    this->raw_in_flight_ = true;
    this->in_flight_started_ms_ = millis();
    ESP_LOGV(TAG, "Dispatched raw %s 0x%04X len %u", op.is_write ? "write" : "read", op.address, op.length);
    this->raw_queue_.pop_front();
  }
}

void VitoHomeComponent::dispatch_next_() {
  if (this->in_flight_ != nullptr || this->ident_in_flight_ || this->raw_in_flight_) return;

  // Identification runs before regular traffic so the user sees the device
  // tuple in the first seconds of the log.
  if (this->ident_state_ != IdentState::IDLE && this->ident_state_ != IdentState::DONE) {
    if (this->vito_->read(this->ident_dp_)) {
      this->ident_in_flight_ = true;
      this->in_flight_started_ms_ = millis();
      ESP_LOGV(TAG, "Dispatched identification read 0x%04X len %u", this->ident_dp_.address(),
               this->ident_dp_.length());
    }
    return;
  }

  // Interactive scan-console ops (RawPurpose::SCAN) preempt regular polling
  // -- and a queued user write -- so range sweeps in the scan console feel
  // immediate. This is the only raw purpose allowed to jump ahead of
  // write_queue_; CLOCK_* is handled below, after writes.
  if (!this->raw_queue_.empty() && this->raw_queue_.front().purpose == RawPurpose::SCAN) {
    this->dispatch_raw_front_();
    return;  // engine busy: retry next loop()
  }

  // Writes preempt reads: a user-initiated setpoint change should not wait
  // behind a full poll cycle. This also preempts a queued background
  // clock-sync step (RawPurpose::CLOCK_READ/CLOCK_WRITE/CLOCK_VERIFY, checked
  // below): unlike the scan console, clock sync is not something a person is
  // waiting on, and a full sync can take up to three sequential round trips
  // (read, conditional write, verify-read), so letting it jump the write
  // queue could stall a user's slider drag across several bus transactions
  // at 4800 baud. A one-dispatch-cycle delay to a clock step is invisible;
  // a multi-second delay to a write is not.
  if (!this->write_queue_.empty()) {
    VitoEntityBase* entity = this->write_queue_.front();
    if (this->vito_->write(entity->get_write_datapoint(), entity->write_data(), entity->write_length())) {
      this->in_flight_ = entity;
      this->in_flight_op_ = OpType::WRITE;
      this->in_flight_started_ms_ = millis();
      this->write_queue_.pop_front();
      // It has left the queue and is now in flight. Clearing write_queued_ here
      // (rather than on completion) lets a value changed during the in-flight
      // window re-enqueue, so the newest payload is still transmitted.
      entity->write_queued_ = false;
      entity->write_in_flight_ = true;
      ESP_LOGV(TAG, "Dispatched write for %s (%u bytes)", entity->get_datapoint().name(), entity->write_length());
    }
    return;  // engine busy: retry next loop()
  }

  // Background clock-sync ops get a turn once there is no pending user write.
  // (raw_queue_ can only hold CLOCK_* purposes here -- SCAN was already
  // dispatched above if present.)
  if (!this->raw_queue_.empty()) {
    this->dispatch_raw_front_();
    return;  // engine busy: retry next loop()
  }

  if (this->read_queue_.empty()) return;
  VitoEntityBase* entity = this->read_queue_.front();
  if (this->vito_->read(entity->get_datapoint())) {
    this->in_flight_ = entity;
    this->in_flight_op_ = OpType::READ;
    this->in_flight_started_ms_ = millis();
    this->read_queue_.pop_front();
    ESP_LOGV(TAG, "Dispatched read for %s", entity->get_datapoint().name());
  }
  // else: optolink engine is busy with internal state; retry next loop().
}

void VitoHomeComponent::schedule_due_entities_() {
  const uint32_t now = millis();
  size_t queued = 0, skipped = 0;
  for (auto* entity : this->entities_) {
    if (entity->read_queued_) {
      skipped++;
      continue;  // still waiting from a previous cycle — don't double-queue
    }
    if (entity->poll_interval() != 0) {
      // next_due_ms_ == 0 means "never polled": due immediately.
      if (entity->next_due_ms_ != 0 && static_cast<int32_t>(now - entity->next_due_ms_) < 0) continue;
      entity->next_due_ms_ = now + entity->poll_interval();
    }
    entity->read_queued_ = true;
    this->read_queue_.push_back(entity);
    queued++;
  }
  if (skipped > 0) {
    ESP_LOGW(TAG, "Poll cycle: %zu entities still queued from the previous cycle (bus saturated?)", skipped);
  }
  ESP_LOGV(TAG, "Queued %zu reads", queued);
}

void VitoHomeComponent::update() {
  if (this->vito_ == nullptr) return;
  this->time_sync_tick_();
  if (this->entities_.empty()) return;
  this->schedule_due_entities_();
}

void VitoHomeComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "VitoHome:");
  ESP_LOGCONFIG(TAG, "  Protocol: %s", ProtocolAdapter::protocol_name());
  ESP_LOGCONFIG(TAG, "  Entities: %zu", this->entities_.size());
  if (this->ident_state_ == IdentState::DONE) {
    ESP_LOGCONFIG(TAG, "  Device: %s", this->ident_string_().c_str());
  }
  if (!this->raw_result_sensors_.empty()) {
    ESP_LOGCONFIG(TAG, "  Scan console: %zu scan_result sensor(s) attached", this->raw_result_sensors_.size());
  }
  this->check_uart_settings(4800, 2, uart::UART_CONFIG_PARITY_EVEN, 8);
  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Setup FAILED");
    return;
  }
  for (auto* e : this->entities_) {
    e->dump_config();
  }
}

bool VitoHomeComponent::request_write(VitoEntityBase* entity) {
  if (entity == nullptr || entity->write_length() == 0) return false;
  if (entity->write_queued_) {
    // Already in the write queue and not yet dispatched: control() has already
    // overwritten the entity buffer with the newest payload, so the pending
    // dispatch will transmit the latest value. Coalesce — nothing to do.
    return true;
  }
  // Not queued. Either idle, or a write for this entity is currently in flight
  // (write_in_flight_). In the in-flight case the dispatched bytes were already
  // handed to the engine, so the buffer now holds a newer value that would be
  // lost; enqueue again so it is transmitted once the in-flight write finishes.
  entity->write_queued_ = true;
  this->write_queue_.push_back(entity);
  return true;
}

// ---------------------------------------------------------------------------
// Raw scan console (debug)
// ---------------------------------------------------------------------------

void VitoHomeComponent::queue_raw_read(uint16_t address, uint8_t length) {
  if (length < 1 || length > 32) {
    ESP_LOGW(TAG, "queue_raw_read: length %u out of range (1..32)", length);
    return;
  }
  this->enqueue_raw_(address, length, false, {}, RawPurpose::SCAN);
}

void VitoHomeComponent::queue_raw_write(uint16_t address, const std::vector<uint8_t>& bytes) {
  if (bytes.empty() || bytes.size() > 32) {
    ESP_LOGW(TAG, "queue_raw_write: %zu bytes out of range (1..32)", bytes.size());
    return;
  }
  this->enqueue_raw_(address, static_cast<uint8_t>(bytes.size()), true, bytes, RawPurpose::SCAN);
}

void VitoHomeComponent::enqueue_raw_(uint16_t address, uint8_t length, bool is_write, const std::vector<uint8_t>& bytes,
                                     RawPurpose purpose) {
  if (this->raw_queue_.size() >= RAW_QUEUE_MAX) {
    ESP_LOGW(TAG, "raw queue full (%zu); dropping %s 0x%04X", this->raw_queue_.size(), is_write ? "write" : "read",
             address);
    return;
  }
  this->raw_queue_.push_back(RawOp{address, length, is_write, bytes, purpose});
  ESP_LOGD(TAG, "Queued raw %s 0x%04X len %u", is_write ? "write" : "read", address, length);
}

void VitoHomeComponent::raw_handle_response_(const ResponseView& response) {
  switch (this->raw_purpose_) {
    case RawPurpose::CLOCK_READ:
      this->clock_handle_read_(response);
      return;
    case RawPurpose::CLOCK_WRITE:
      this->clock_handle_write_ack_();
      return;
    case RawPurpose::CLOCK_VERIFY:
      this->clock_handle_verify_(response);
      return;
    case RawPurpose::SCAN:
    default:
      break;
  }
  char buf[160];
  if (this->raw_is_write_) {
    snprintf(buf, sizeof(buf), "0x%04X: write ACK (%zu byte%s)", this->raw_dp_.address(), this->raw_write_buf_.size(),
             this->raw_write_buf_.size() == 1 ? "" : "s");
  } else {
    format_raw_dump(response.address, response.data, response.data_length, buf, sizeof(buf));
  }
  ESP_LOGI(TAG, "Raw: %s", buf);
  this->raw_publish_(buf);
}

void VitoHomeComponent::raw_handle_error_(optolink::OptolinkResult error) {
  if (this->raw_purpose_ != RawPurpose::SCAN) {
    ESP_LOGW(TAG, "System-time sync: %s 0x%04X failed (%s)", this->raw_is_write_ ? "write" : "read",
             this->raw_dp_.address(), optolink::errorToString(error));
    return;
  }
  char buf[96];
  snprintf(buf, sizeof(buf), "0x%04X: %s FAILED (%s)", this->raw_dp_.address(), this->raw_is_write_ ? "write" : "read",
           optolink::errorToString(error));
  ESP_LOGW(TAG, "Raw: %s", buf);
  this->raw_publish_(buf);
}

void VitoHomeComponent::raw_publish_(const std::string& line) {
  for (auto* ts : this->raw_result_sensors_) ts->publish_state(line);
}

// ---------------------------------------------------------------------------
// System-time sync (rides the raw lane)
// ---------------------------------------------------------------------------
// The now()-using bodies are compiled only when a time source is configured
// (-DVITOHOME_TIME_SYNC, set by to_code when time_id is present), so a build
// without time sync pulls in no dependency on the time component.

void VitoHomeComponent::time_sync_tick_() {
#ifdef VITOHOME_TIME_SYNC
  if (this->time_source_ == nullptr) return;
  const uint32_t now = millis();
  if (!this->time_sync_did_boot_) {
    // Defer the first sync until the time source has a valid time at least once.
    if (!this->time_source_->now().is_valid()) return;
    this->time_sync_did_boot_ = true;
    this->time_sync_next_ms_ = now + this->time_sync_interval_ms_;
    if (this->time_sync_on_boot_) this->sync_system_time_();
    return;
  }
  if (this->time_sync_interval_ms_ != 0 && static_cast<int32_t>(now - this->time_sync_next_ms_) >= 0) {
    this->time_sync_next_ms_ = now + this->time_sync_interval_ms_;
    this->sync_system_time_();
  }
#endif
}

void VitoHomeComponent::sync_system_time_() {
#ifdef VITOHOME_TIME_SYNC
  if (this->time_source_ == nullptr) return;
  if (!this->time_source_->now().is_valid()) {
    ESP_LOGW(TAG, "System-time sync: time source not valid yet, skipping");
    return;
  }
  // Read the device clock first; clock_handle_read_ compares it with the time
  // source and only writes when the drift exceeds the threshold.
  this->enqueue_raw_(CLOCK_ADDRESS, CLOCK_LEN, false, {}, RawPurpose::CLOCK_READ);
#endif
}

void VitoHomeComponent::clock_handle_read_(const ResponseView& response) {
#ifdef VITOHOME_TIME_SYNC
  if (this->time_source_ == nullptr) return;
  const ESPTime t = this->time_source_->now();
  if (!t.is_valid()) {
    ESP_LOGW(TAG, "System-time sync: time source became invalid, skipping");
    return;
  }
  BcdDateTime dev{};
  const bool dev_ok = decode_datetime_bcd(response.data, response.data_length, 0, &dev);
  bool need_write = true;
  if (dev_ok) {
    BcdDateTime ha{};
    ha.year = t.year;
    ha.month = t.month;
    ha.day = t.day_of_month;
    ha.hour = t.hour;
    ha.minute = t.minute;
    ha.second = t.second;
    const int64_t drift = civil_seconds(ha) - civil_seconds(dev);
    const int64_t mag = drift < 0 ? -drift : drift;
    if (mag <= static_cast<int64_t>(this->time_drift_threshold_s_)) {
      ESP_LOGD(TAG, "System-time sync: drift %lds within %us, no write", static_cast<long>(drift),
               this->time_drift_threshold_s_);
      need_write = false;
    } else {
      ESP_LOGI(TAG, "System-time sync: drift %lds exceeds %us, updating device clock", static_cast<long>(drift),
               this->time_drift_threshold_s_);
    }
  } else {
    ESP_LOGI(TAG, "System-time sync: device clock unreadable, setting it");
  }
  if (!need_write) return;
  uint8_t buf[CLOCK_LEN];
  const uint8_t weekday = device_weekday_from_esptime(t.day_of_week);
  if (!encode_datetime_bcd(t.year, t.month, t.day_of_month, weekday, t.hour, t.minute, t.second, buf)) {
    ESP_LOGW(TAG, "System-time sync: time source out of range, skipping");
    return;
  }
  this->enqueue_raw_(CLOCK_ADDRESS, CLOCK_LEN, true, std::vector<uint8_t>(buf, buf + CLOCK_LEN),
                     RawPurpose::CLOCK_WRITE);
#else
  (void)response;
#endif
}

void VitoHomeComponent::clock_handle_write_ack_() {
  ESP_LOGI(TAG, "System-time sync: device clock set; reading back to confirm");
  this->enqueue_raw_(CLOCK_ADDRESS, CLOCK_LEN, false, {}, RawPurpose::CLOCK_VERIFY);
}

void VitoHomeComponent::clock_handle_verify_(const ResponseView& response) {
  BcdDateTime dev{};
  if (decode_datetime_bcd(response.data, response.data_length, 0, &dev)) {
    ESP_LOGI(TAG, "System-time sync: device clock now %04u-%02u-%02u %02u:%02u:%02u", dev.year, dev.month, dev.day,
             dev.hour, dev.minute, dev.second);
  } else {
    ESP_LOGW(TAG, "System-time sync: read-back of device clock unreadable");
  }
}

void VitoHomeComponent::on_response_(const ResponseView& response, const optolink::Datapoint& request) {
  if (this->ident_in_flight_) {
    this->ident_in_flight_ = false;
    this->ident_handle_response_(response);
    return;
  }

  if (this->raw_in_flight_) {
    this->raw_in_flight_ = false;
    this->raw_handle_response_(response);
    return;
  }

  VitoEntityBase* entity = this->in_flight_;
  OpType op = this->in_flight_op_;
  this->in_flight_ = nullptr;
  this->in_flight_op_ = OpType::NONE;
  if (entity == nullptr) {
    ESP_LOGW(TAG, "Response received for 0x%04X but no in-flight request", request.address());
    return;
  }
  // A write was dispatched to the command address; a read to the state
  // address. Match the response against whichever this op used (they differ
  // only for two-address controls; otherwise both are datapoint_). The
  // response.address is the address echoed in the device's own frame on P300
  // (see ProtocolAdapter), so this is a live wire-level check there; on
  // KW/GWG the adapter fills it from the request and the check is a no-op.
  // (It previously compared request.address() against the entity's own
  // datapoint -- the same value by construction -- and could never fire.)
  const uint16_t expected_addr =
      (op == OpType::WRITE) ? entity->get_write_datapoint().address() : entity->get_datapoint().address();
  if (expected_addr != response.address) {
    ESP_LOGW(TAG, "Response address 0x%04X does not match in-flight 0x%04X; dropping", response.address, expected_addr);
    // Clear only the state that belongs to THIS op: a stray write-path clear
    // of read_queued_ could let a still-queued poll read be double-queued. If
    // a newer value re-enqueued during this transaction, write_queued_ stays
    // set so it is still transmitted.
    if (op == OpType::READ) {
      entity->read_queued_ = false;
    } else {
      entity->write_in_flight_ = false;
    }
    return;
  }

  if (op == OpType::WRITE) {
    entity->write_in_flight_ = false;
    ESP_LOGD(TAG, "Write to %s acknowledged", entity->get_datapoint().name());
    entity->handle_write_response(response);
    if (entity->wants_read_back() && !entity->read_queued_) {
      // Confirm by reading the device's view of the value, ahead of the
      // regular poll queue.
      entity->read_queued_ = true;
      this->read_queue_.push_front(entity);
    }
    return;
  }

  entity->read_queued_ = false;
  entity->handle_response(response);
}

void VitoHomeComponent::on_error_(optolink::OptolinkResult error, const optolink::Datapoint& request) {
  if (this->ident_in_flight_) {
    this->ident_in_flight_ = false;
    ESP_LOGD(TAG, "Identification read 0x%04X len %u failed (%s)", request.address(), request.length(),
             optolink::errorToString(error));
    this->ident_handle_error_();
    return;
  }

  if (this->raw_in_flight_) {
    this->raw_in_flight_ = false;
    this->raw_handle_error_(error);
    return;
  }

  VitoEntityBase* entity = this->in_flight_;
  OpType op = this->in_flight_op_;
  this->in_flight_ = nullptr;
  this->in_flight_op_ = OpType::NONE;

  const char* name = request.name();
  switch (error) {
    case optolink::OptolinkResult::TIMEOUT:
      ESP_LOGE(TAG, "[TIMEOUT] %s — Optolink not responding", name);
      break;
    case optolink::OptolinkResult::LENGTH:
      ESP_LOGE(TAG, "[LENGTH]  %s — invalid payload length", name);
      break;
    case optolink::OptolinkResult::NACK:
      ESP_LOGW(TAG,
               "[NACK]    %s — heater rejected request "
               "(unsupported address?)",
               name);
      break;
    case optolink::OptolinkResult::CRC:
      ESP_LOGE(TAG, "[CRC]     %s — checksum mismatch (wiring?)", name);
      break;
    case optolink::OptolinkResult::ERROR:
    default:
      ESP_LOGE(TAG, "[ERROR]   %s — protocol error", name);
      break;
  }
  if (entity != nullptr) {
    if (op == OpType::READ) {
      entity->read_queued_ = false;
      entity->handle_error(error);
    } else {
      entity->write_in_flight_ = false;
      // The device value did not change on a failed write; the entity keeps
      // its state (default no-op) rather than going unavailable.
      entity->handle_write_error(error);
    }
  }
}

// ---------------------------------------------------------------------------
// Identification
// ---------------------------------------------------------------------------

void VitoHomeComponent::ident_start_() {
  this->ident_state_ = IdentState::READ4;
  this->ident_dispatch_(IdentState::READ4);
}

void VitoHomeComponent::ident_dispatch_(IdentState state) {
  this->ident_state_ = state;
  switch (state) {
    case IdentState::READ4:
      this->ident_dp_ = optolink::Datapoint("ident", 0x00F8, 4, optolink::noconv);
      break;
    case IdentState::READ_F8:
      this->ident_dp_ = optolink::Datapoint("ident", 0x00F8, 1, optolink::noconv);
      break;
    case IdentState::READ_F9:
      this->ident_dp_ = optolink::Datapoint("ident", 0x00F9, 1, optolink::noconv);
      break;
    case IdentState::READ_FA:
      this->ident_dp_ = optolink::Datapoint("ident", 0x00FA, 1, optolink::noconv);
      break;
    case IdentState::READ_FB:
      this->ident_dp_ = optolink::Datapoint("ident", 0x00FB, 1, optolink::noconv);
      break;
    default:
      break;
  }
  // The actual bus dispatch happens from dispatch_next_() when idle.
}

void VitoHomeComponent::ident_handle_response_(const ResponseView& response) {
  const uint8_t* d = response.data;
  const uint8_t n = response.data_length;
  switch (this->ident_state_) {
    case IdentState::READ4:
      // 0xF8..0xFB in one transaction. Wire order is the register order:
      // F8 = group, F9 = controller, FA = HW index, FB = SW index.
      if (n >= 4) {
        this->ident_group_ = d[0];
        this->ident_controller_ = d[1];
        this->ident_hw_ = d[2];
        this->ident_sw_ = d[3];
        this->ident_finish_();
        return;
      }
      // Short response: fall back to single-byte reads.
      this->ident_dispatch_(IdentState::READ_F8);
      return;
    case IdentState::READ_F8:
      if (n >= 1) this->ident_group_ = d[0];
      this->ident_dispatch_(IdentState::READ_F9);
      return;
    case IdentState::READ_F9:
      if (n >= 1) this->ident_controller_ = d[0];
      this->ident_dispatch_(IdentState::READ_FA);
      return;
    case IdentState::READ_FA:
      if (n >= 1) this->ident_hw_ = d[0];
      this->ident_dispatch_(IdentState::READ_FB);
      return;
    case IdentState::READ_FB:
      if (n >= 1) this->ident_sw_ = d[0];
      this->ident_finish_();
      return;
    default:
      return;
  }
}

void VitoHomeComponent::ident_handle_error_() {
  // Fail-soft per step: the multi-byte read degrades to single-byte reads
  // (length-1 reads at F8/F9 are wire-confirmed on the reference unit), and
  // each single-byte failure just leaves that field unknown.
  switch (this->ident_state_) {
    case IdentState::READ4:
      this->ident_dispatch_(IdentState::READ_F8);
      return;
    case IdentState::READ_F8:
      this->ident_dispatch_(IdentState::READ_F9);
      return;
    case IdentState::READ_F9:
      this->ident_dispatch_(IdentState::READ_FA);
      return;
    case IdentState::READ_FA:
      this->ident_dispatch_(IdentState::READ_FB);
      return;
    case IdentState::READ_FB:
      this->ident_finish_();
      return;
    default:
      return;
  }
}

std::string VitoHomeComponent::ident_string_() const {
  char buf[96];
  if (this->ident_group_ >= 0 && this->ident_controller_ >= 0) {
    const uint16_t ident = static_cast<uint16_t>((this->ident_group_ << 8) | this->ident_controller_);
    const char* family = ident_family_name(ident);
    int off = snprintf(buf, sizeof(buf), "0x%04X%s%s%s", ident, family != nullptr ? " (" : "",
                       family != nullptr ? family : "", family != nullptr ? ")" : "");
    if (this->ident_hw_ >= 0 && this->ident_sw_ >= 0 && off > 0 && off < static_cast<int>(sizeof(buf))) {
      snprintf(buf + off, sizeof(buf) - off, " HW=0x%02X SW=0x%02X", this->ident_hw_, this->ident_sw_);
    }
    return std::string(buf);
  }
  return std::string("unknown (identification reads failed)");
}

void VitoHomeComponent::ident_finish_() {
  this->ident_state_ = IdentState::DONE;
  const std::string s = this->ident_string_();
  ESP_LOGI(TAG, "Device identification: %s", s.c_str());
  if (this->ident_sw_ < 0 && this->ident_group_ >= 0) {
    ESP_LOGI(TAG,
             "Software index (0xFB) unavailable — when picking datapoints from the "
             "Vitosoft data, match on the family only and verify on the wire.");
  }
  for (auto* ts : this->device_id_sensors_) {
    ts->publish_state(s);
  }
}

}  // namespace vitohome
}  // namespace esphome
