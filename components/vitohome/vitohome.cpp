#include "vitohome.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "decode.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "poll_schedule.h"
#ifdef VITOHOME_TIME_SYNC
#include "esphome/components/time/real_time_clock.h"
#endif

namespace esphome::vitohome {

static const char *const TAG = "vitohome";

// Known device families (Identification at 0xF8/0xF9). Deliberately small:
// it covers the units this project has seen on the wire; everything else is
// reported as raw hex, and the catalogue tooling (scripts/gen_catalog.py)
// does the authoritative matching against the Vitosoft data.
static const char *ident_family_name(uint16_t ident) {
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
  if (this->is_failed())
    return;

  // Size the run-loop queues once, now that registration is complete (the
  // entity-count log below is proof). Each lane makes exactly one allocation
  // here and never allocates again: the read/write lanes to the registered
  // entity count (an entity is enqueued at most once, so that is their true
  // ceiling and they can never fill), the raw lane to its scan-sweep cap. After
  // this, run-loop queue traffic is heap-free. A failure here is out of heap at
  // boot -- fail loudly rather than run without working queues.
  if (!this->read_queue_.reserve(this->entities_.size()) || !this->write_queue_.reserve(this->entities_.size()) ||
      !this->raw_queue_.reserve(RAW_QUEUE_MAX)) {
    ESP_LOGE(TAG, "failed to allocate poll queues for %zu entities (out of heap)", this->entities_.size());
    this->mark_failed();
    return;
  }

  // The engine is build-time-selected (protocol_select.h) and deduces the
  // interface type, wrapping &iface_ in a GenericInterface internally.
  this->vito_ = std::make_unique<optolink::OptolinkEngine<SelectedProtocol>>(&this->iface_);

  // All three engines share one byte-mover callback shape, so the hub registers
  // directly with the engine and wraps the raw payload in a ResponseView here.
  // On P300 `address` is the one echoed in the device's own response frame (a
  // real wire-level datum); on KW/GWG the engine echoes the retained request
  // address. The engine is strictly single-in-flight and the hub tracks its own
  // in-flight context (in_flight_ / ident_in_flight_ / raw_in_flight_), so the
  // callback carries only that address as a wire-level cross-check.
  this->vito_->onResponse([this](const uint8_t *data, uint8_t length, uint16_t address) {
    // Any successful response is proof of link liveness -- and of the
    // configured protocol (start-up verification below).
    this->link_established_ = true;
    this->link_error_streak_ = 0;
    this->publish_link_(true);
    this->on_response_(ResponseView{data, length, address}, address);
  });
  this->vito_->onError([this](optolink::OptolinkResult error, uint16_t request_address) {
    // Link health tracks a real, persistent link-down -- not every protocol
    // hiccup. A NACK (the device actively transmitted a negative ack) and a
    // device ERROR frame (a complete, link-layer-acked reply) both PROVE the
    // optical link is alive: the device received the request and answered. Only
    // a TIMEOUT -- no bytes at all within the watchdog -- indicates the link
    // itself may be down, so only TIMEOUT feeds the offline streak. Feeding
    // NACK/ERROR here made the connectivity sensor flap offline whenever a few
    // consecutively-polled addresses were unsupported (e.g. a scan sweep or a
    // generated catalog with NAKing addresses). CRC (a received-but-corrupt
    // frame) is transient bus noise and is likewise not counted.
    if (error == optolink::OptolinkResult::TIMEOUT) {
      this->link_note_error_();
    }
    this->on_error_(error, request_address);
  });

  if (!this->vito_->begin()) {
    ESP_LOGE(TAG, "optolink engine begin() failed");
    this->mark_failed();
    return;
  }

  // Require the configured protocol to establish a link within the start-up
  // window (max of the floor and 3x the hub interval); loop() marks the
  // component failed otherwise.
  uint32_t verify_window = 3u * this->get_update_interval();
  if (verify_window < PROTOCOL_VERIFY_MIN_MS)
    verify_window = PROTOCOL_VERIFY_MIN_MS;
  this->protocol_verify_pending_ = true;
  // A one-shot setup anchor, not a hot-loop read. App.get_loop_component_start_time()
  // exists to avoid repeated slow millis() reads on hot paths; that benefit does not
  // apply to a single setup-time deadline, so a fresh read is kept deliberately.
  this->protocol_verify_deadline_ms_ = millis() + verify_window;

  // Per-entity intervals are scheduled at hub-tick granularity, so anything
  // shorter than the hub interval silently degrades to the hub interval.
  // Surface that at setup instead of letting the user chase phantom lag.
  const uint32_t hub_interval = this->get_update_interval();
  for (auto *e : this->entities_) {
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
  auto *bus = this->parent_;
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
  if (this->vito_ == nullptr)
    return;

  this->vito_->loop();

  // Frame logging (compile-time; see vito_uart_interface.h). TX frames are
  // emitted from write(); this closes an RX frame once the bus goes quiet.
  // Compiles to nothing without -DVITOHOME_LOG_FRAMES.
  this->iface_.frame_tick();

  // Start-up protocol verification: confirm the configured protocol actually
  // established a link, or fail the component with a clear message.
  if (this->protocol_verify_pending_) {
    if (this->link_established_) {
      this->protocol_verify_pending_ = false;
      ESP_LOGI(TAG, "%s link established", PROTOCOL_NAME);
    } else if (static_cast<int32_t>(App.get_loop_component_start_time() - this->protocol_verify_deadline_ms_) >= 0) {
      // Rollover-safe signed-diff compare, matching the scheduler and the
      // time-sync tick (a direct >= compares wrong across the 49.7-day wrap).
      this->protocol_verify_pending_ = false;
      ESP_LOGE(TAG, "%s link not established; check wiring and that the device speaks this protocol", PROTOCOL_NAME);
      this->publish_link_(false);
      this->mark_failed();
    }
  }

  // Watchdog: if a request has been in flight too long, surface that and
  // free the slot.
  if (this->in_flight_ != nullptr || this->ident_in_flight_ || this->raw_in_flight_) {
    uint32_t now = App.get_loop_component_start_time();
    if (now - this->in_flight_started_ms_ > IN_FLIGHT_WATCHDOG_MS) {
      // A lost engine callback is a link-health signal too.
      this->link_note_error_();
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
  const RawOp &op = this->raw_queue_.front();
  this->raw_dp_ =
      optolink::Datapoint(op.purpose == RawPurpose::SCAN ? "scan" : "clock", op.address, op.length, optolink::noconv);
  this->raw_is_write_ = op.is_write;
  this->raw_purpose_ = op.purpose;
  bool dispatched;
  if (op.is_write) {
    // The engine serializes op.bytes into its own packet buffer inside
    // write(), so the payload need not outlive this call; only the length is
    // retained (raw_write_len_) for the ack log line.
    dispatched = this->vito_->write(this->raw_dp_.address(), op.bytes, op.bytes_len);
  } else {
    dispatched = this->vito_->read(this->raw_dp_.address(), this->raw_dp_.length());
  }
  if (dispatched) {
    this->raw_write_len_ = op.is_write ? op.bytes_len : 0;
    this->raw_in_flight_ = true;
    this->in_flight_started_ms_ = App.get_loop_component_start_time();
    ESP_LOGV(TAG, "Dispatched raw %s 0x%04X len %u", op.is_write ? "write" : "read", op.address, op.length);
    this->raw_queue_.pop_front();
  }
}

void VitoHomeComponent::dispatch_next_() {
  if (this->in_flight_ != nullptr || this->ident_in_flight_ || this->raw_in_flight_)
    return;

  // Identification runs before regular traffic so the user sees the device
  // tuple in the first seconds of the log.
  if (this->ident_state_ != IdentState::IDLE && this->ident_state_ != IdentState::DONE) {
    if (this->vito_->read(this->ident_dp_.address(), this->ident_dp_.length())) {
      this->ident_in_flight_ = true;
      this->in_flight_started_ms_ = App.get_loop_component_start_time();
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
    VitoEntityBase *entity = this->write_queue_.front();
    if (this->vito_->write(entity->get_write_datapoint().address(), entity->write_data(), entity->write_length())) {
      this->in_flight_ = entity;
      this->in_flight_op_ = OpType::WRITE;
      this->in_flight_started_ms_ = App.get_loop_component_start_time();
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

  if (this->read_queue_.empty())
    return;
  VitoEntityBase *entity = this->read_queue_.front();
  const optolink::Datapoint &dp = entity->get_datapoint();
  if (this->vito_->read(dp.address(), dp.length())) {
    this->in_flight_ = entity;
    this->in_flight_op_ = OpType::READ;
    this->in_flight_started_ms_ = App.get_loop_component_start_time();
    this->read_queue_.pop_front();
    ESP_LOGV(TAG, "Dispatched read for %s", entity->get_datapoint().name());
  }
  // else: optolink engine is busy with internal state; retry next loop().
}

void VitoHomeComponent::schedule_due_entities_() {
  const uint32_t now = App.get_loop_component_start_time();
  // Two bugs lived in the old two-liner (`if (now < next_due) continue;
  // next_due = now + interval;`), both hardware-observed on VScotHO1_72 with
  // the SAME firmware binary across two 2026-07-09 logs:
  //
  //  1. `now` is sampled inside this callback, i.e. a few ms AFTER the
  //     ESPHome interval anchor that invoked update(). Re-anchoring the next
  //     due time on it made an entity whose update_interval EQUALS the hub
  //     tick (or an exact multiple of it) land a hair past the next tick, so
  //     whether it fired was decided by which tick carried more loop jitter --
  //     a coin flip. One log dropped the whole 60 s tier on tick 2; the other
  //     never dropped it. Anchoring on next_due_ms_ instead of `now` makes the
  //     schedule an arithmetic progression that cannot drift.
  //  2. Even when it did fire, the period crept: each cycle added the
  //     accumulated jitter into the next due time.
  //
  // SLACK absorbs sub-tick jitter in the "is it due yet" test: anything due
  // within half a hub tick of now is treated as due now, because the next
  // opportunity to poll it is a full hub tick away and firing a few ms early
  // beats firing a whole tick late.
  const uint32_t slack = this->get_update_interval() / 2;
  size_t queued = 0, skipped = 0;
  for (auto *entity : this->entities_) {
    if (entity->read_queued_) {
      skipped++;
      continue;  // still waiting from a previous cycle — don't double-queue
    }
    const PollDecision d = poll_schedule_step(now, entity->next_due_ms_, entity->poll_interval(), slack);
    if (!d.due)
      continue;
    if (entity->poll_interval() != 0)
      entity->next_due_ms_ = d.next_due_ms;
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
  if (this->vito_ == nullptr)
    return;
  this->time_sync_tick_();
  if (this->entities_.empty())
    return;
  this->schedule_due_entities_();
}

void VitoHomeComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "VitoHome:");
  ESP_LOGCONFIG(TAG, "  Protocol: %s", PROTOCOL_NAME);
  ESP_LOGCONFIG(TAG, "  Entities: %zu", this->entities_.size());
  if (this->ident_state_ == IdentState::DONE) {
    ESP_LOGCONFIG(TAG, "  Device: %s", this->ident_string_().c_str());
  }
#ifdef USE_TEXT_SENSOR
  if (!this->raw_result_sensors_.empty()) {
    ESP_LOGCONFIG(TAG, "  Scan console: %zu scan_result sensor(s) attached", this->raw_result_sensors_.size());
  }
#endif
  if (ESPHomeUARTInterface::frame_logging_enabled()) {
    ESP_LOGCONFIG(TAG, "  Frame logging: ON (tag 'vitohome.frames')");
  }
  this->check_uart_settings(4800, 2, uart::UART_CONFIG_PARITY_EVEN, 8);
  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Setup FAILED");
  }
  // Deliberately NO loop over entities_ here: every entity is a registered
  // component, so ESPHome core already calls each one's dump_config() --
  // the old loop printed the entire entity list twice at boot
  // (hardware-observed, 2026-07-03 log).
}

bool VitoHomeComponent::request_write(VitoEntityBase *entity) {
  if (entity == nullptr || entity->write_length() == 0)
    return false;
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
  if (length < 1 || length > RAW_READ_MAX) {
    ESP_LOGW(TAG, "queue_raw_read: length %u out of range (1..%u)", length, RAW_READ_MAX);
    return;
  }
  this->enqueue_raw_(address, length, false, nullptr, 0, RawPurpose::SCAN);
}

void VitoHomeComponent::queue_raw_write(uint16_t address, const std::vector<uint8_t> &bytes) {
  // The 32-byte cap also keeps the packet length() arithmetic safe: the VS2
  // length byte is 0x05 + len (uint8_t, wraps for len > 250) and the VS1
  // frame length is payload + 4 (wraps for len >= 252) -- see the comments at
  // PacketVS2::length() / PacketVS1::length() before ever raising this cap.
  if (bytes.empty() || bytes.size() > RAW_WRITE_MAX) {
    ESP_LOGW(TAG, "queue_raw_write: %zu bytes out of range (1..%u)", bytes.size(), RAW_WRITE_MAX);
    return;
  }
  this->enqueue_raw_(address, static_cast<uint8_t>(bytes.size()), true, bytes.data(),
                     static_cast<uint8_t>(bytes.size()), RawPurpose::SCAN);
}

void VitoHomeComponent::enqueue_raw_(uint16_t address, uint8_t length, bool is_write, const uint8_t *bytes,
                                     uint8_t bytes_len, RawPurpose purpose) {
  if (this->raw_queue_.full()) {
    ESP_LOGW(TAG, "raw queue full (%zu); dropping %s 0x%04X", this->raw_queue_.size(), is_write ? "write" : "read",
             address);
    return;
  }
  if (bytes_len > RAW_WRITE_MAX) {  // callers cap earlier; defend anyway
    ESP_LOGW(TAG, "enqueue_raw_: %u bytes exceeds %u; dropping", bytes_len, RAW_WRITE_MAX);
    return;
  }
  RawOp op{};
  op.address = address;
  op.length = length;
  op.is_write = is_write;
  op.bytes_len = bytes_len;
  if (bytes != nullptr && bytes_len > 0) {
    std::memcpy(op.bytes, bytes, bytes_len);
  }
  op.purpose = purpose;
  this->raw_queue_.push_back(op);
  ESP_LOGD(TAG, "Queued raw %s 0x%04X len %u", is_write ? "write" : "read", address, length);
}

void VitoHomeComponent::raw_handle_response_(const ResponseView &response) {
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
  // "0xXXXX:" (7) + RAW_READ_MAX * 3 hex chars + the integer views for widths
  // 1..8 + NUL. 208 covers a 48-byte dump with room to spare; format_raw_dump()
  // truncates safely rather than overflowing, but truncating a scan result would
  // silently lose the bytes the user asked for.
  char buf[208];
  if (this->raw_is_write_) {
    snprintf(buf, sizeof(buf), "0x%04X: write ACK (%u byte%s)", this->raw_dp_.address(), this->raw_write_len_,
             this->raw_write_len_ == 1 ? "" : "s");
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

void VitoHomeComponent::raw_publish_(const std::string &line) {
#ifdef USE_TEXT_SENSOR
  for (auto *ts : this->raw_result_sensors_)
    ts->publish_state(line);
#else
  (void) line;
#endif
}

// ---------------------------------------------------------------------------
// System-time sync (rides the raw lane)
// ---------------------------------------------------------------------------
// The now()-using bodies are compiled only when a time source is configured
// (-DVITOHOME_TIME_SYNC, set by to_code when time_id is present), so a build
// without time sync pulls in no dependency on the time component.

void VitoHomeComponent::time_sync_tick_() {
#ifdef VITOHOME_TIME_SYNC
  if (this->time_source_ == nullptr)
    return;
  const uint32_t now = App.get_loop_component_start_time();
  if (!this->time_sync_did_boot_) {
    // Defer the first sync until the time source has a valid time at least once.
    if (!this->time_source_->now().is_valid())
      return;
    this->time_sync_did_boot_ = true;
    this->time_sync_next_ms_ = now + this->time_sync_interval_ms_;
    if (this->time_sync_on_boot_)
      this->sync_system_time_();
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
  if (this->time_source_ == nullptr)
    return;
  if (!this->time_source_->now().is_valid()) {
    ESP_LOGW(TAG, "System-time sync: time source not valid yet, skipping");
    return;
  }
  // Read the device clock first; clock_handle_read_ compares it with the time
  // source and only writes when the drift exceeds the threshold.
  this->enqueue_raw_(CLOCK_ADDRESS, CLOCK_LEN, false, nullptr, 0, RawPurpose::CLOCK_READ);
#endif
}

void VitoHomeComponent::clock_handle_read_(const ResponseView &response) {
#ifdef VITOHOME_TIME_SYNC
  if (this->time_source_ == nullptr)
    return;
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
  if (!need_write)
    return;
  uint8_t buf[CLOCK_LEN];
  const uint8_t weekday = device_weekday_from_esptime(t.day_of_week);
  if (!encode_datetime_bcd(t.year, t.month, t.day_of_month, weekday, t.hour, t.minute, t.second, buf)) {
    ESP_LOGW(TAG, "System-time sync: time source out of range, skipping");
    return;
  }
  this->enqueue_raw_(CLOCK_ADDRESS, CLOCK_LEN, true, buf, CLOCK_LEN, RawPurpose::CLOCK_WRITE);
#else
  (void) response;
#endif
}

void VitoHomeComponent::clock_handle_write_ack_() {
  ESP_LOGI(TAG, "System-time sync: device clock set; reading back to confirm");
  this->enqueue_raw_(CLOCK_ADDRESS, CLOCK_LEN, false, nullptr, 0, RawPurpose::CLOCK_VERIFY);
}

void VitoHomeComponent::clock_handle_verify_(const ResponseView &response) {
  BcdDateTime dev{};
  if (decode_datetime_bcd(response.data, response.data_length, 0, &dev)) {
    ESP_LOGI(TAG, "System-time sync: device clock now %04u-%02u-%02u %02u:%02u:%02u", dev.year, dev.month, dev.day,
             dev.hour, dev.minute, dev.second);
  } else {
    ESP_LOGW(TAG, "System-time sync: read-back of device clock unreadable");
  }
}

void VitoHomeComponent::on_response_(const ResponseView &response, uint16_t request_address) {
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

  VitoEntityBase *entity = this->in_flight_;
  OpType op = this->in_flight_op_;
  this->in_flight_ = nullptr;
  this->in_flight_op_ = OpType::NONE;
  if (entity == nullptr) {
    ESP_LOGW(TAG, "Response received for 0x%04X but no in-flight request", request_address);
    return;
  }
  // A write was dispatched to the command address; a read to the state
  // address. Match the response against whichever this op used (they differ
  // only for two-address controls; otherwise both are datapoint_). The
  // response.address is the address echoed in the device's own frame on P300
  // (see the onResponse registration in setup()), so this is a live wire-level
  // check there; on KW/GWG the engine echoes the request address and the check
  // is a no-op.
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

bool VitoHomeComponent::refresh_all() {
  // Fresh millis() is deliberate here (not App.get_loop_component_start_time()):
  // refresh_all() is a rare cold path entered from a button press or a user
  // lambda -- i.e. from another component's loop dispatch, not the hub's -- so it
  // is neither hot nor guaranteed to run in the hub's own dispatch context. A
  // single fresh read is the correct anchor for this rate-limit guard.
  const uint32_t now = millis();
  if (this->last_refresh_all_ms_ != 0 && now - this->last_refresh_all_ms_ < REFRESH_ALL_MIN_INTERVAL_MS) {
    ESP_LOGW(TAG, "refresh_all() suppressed (last one %" PRIu32 " ms ago, min interval %" PRIu32 " ms)",
             now - this->last_refresh_all_ms_, REFRESH_ALL_MIN_INTERVAL_MS);
    return false;
  }
  this->last_refresh_all_ms_ = now;
  for (auto *entity : this->entities_) {
    entity->next_due_ms_ = 0;  // boot sentinel: due on the next tick
  }
  ESP_LOGI(TAG, "refresh_all(): %u entities marked due; queue drains at normal pace",
           static_cast<unsigned>(this->entities_.size()));
  return true;
}

void VitoHomeComponent::publish_link_(bool up) {
  const int8_t next = up ? 1 : 0;
  if (this->link_state_ == next)
    return;
  this->link_state_ = next;
  ESP_LOGI(TAG, "Optolink link %s", up ? "online" : "offline");
#ifdef USE_BINARY_SENSOR
  for (auto *bs : this->link_sensors_)
    bs->publish_state(up);
#endif
}

void VitoHomeComponent::link_note_error_() {
  if (this->link_error_streak_ < LINK_OFFLINE_AFTER_ERRORS)
    this->link_error_streak_++;
  if (this->link_error_streak_ == LINK_OFFLINE_AFTER_ERRORS)
    this->publish_link_(false);
}

void VitoHomeComponent::on_error_(optolink::OptolinkResult error, uint16_t request_address) {
  if (this->ident_in_flight_) {
    this->ident_in_flight_ = false;
    ESP_LOGD(TAG, "Identification read 0x%04X failed (%s)", request_address, optolink::errorToString(error));
    this->ident_handle_error_();
    return;
  }

  if (this->raw_in_flight_) {
    this->raw_in_flight_ = false;
    this->raw_handle_error_(error);
    return;
  }

  VitoEntityBase *entity = this->in_flight_;
  OpType op = this->in_flight_op_;
  this->in_flight_ = nullptr;
  this->in_flight_op_ = OpType::NONE;

  // Name for the log line: the in-flight entity's datapoint when there is one,
  // else the echoed request address (a stray error with no in-flight op).
  const char *name = (entity != nullptr) ? entity->get_datapoint().name() : "?";
  switch (error) {
    case optolink::OptolinkResult::TIMEOUT:
      ESP_LOGE(TAG, "[TIMEOUT] %s (0x%04X) — Optolink not responding", name, request_address);
      break;
    case optolink::OptolinkResult::LENGTH:
      ESP_LOGE(TAG, "[LENGTH]  %s (0x%04X) — invalid payload length", name, request_address);
      break;
    case optolink::OptolinkResult::NACK:
      ESP_LOGW(TAG,
               "[NACK]    %s (0x%04X) — heater rejected request "
               "(unsupported address?)",
               name, request_address);
      break;
    case optolink::OptolinkResult::CRC:
      ESP_LOGE(TAG, "[CRC]     %s (0x%04X) — checksum mismatch (wiring?)", name, request_address);
      break;
    case optolink::OptolinkResult::ERROR:
    default:
      ESP_LOGE(TAG, "[ERROR]   %s (0x%04X) — protocol error", name, request_address);
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

void VitoHomeComponent::ident_handle_response_(const ResponseView &response) {
  const uint8_t *d = response.data;
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
      if (n >= 1)
        this->ident_group_ = d[0];
      this->ident_dispatch_(IdentState::READ_F9);
      return;
    case IdentState::READ_F9:
      if (n >= 1)
        this->ident_controller_ = d[0];
      this->ident_dispatch_(IdentState::READ_FA);
      return;
    case IdentState::READ_FA:
      if (n >= 1)
        this->ident_hw_ = d[0];
      this->ident_dispatch_(IdentState::READ_FB);
      return;
    case IdentState::READ_FB:
      if (n >= 1)
        this->ident_sw_ = d[0];
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
    const char *family = ident_family_name(ident);
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
    ESP_LOGI(TAG, "Software index (0xFB) unavailable — when picking datapoints from the "
                  "Vitosoft data, match on the family only and verify on the wire.");
  }
#ifdef USE_TEXT_SENSOR
  for (auto *ts : this->device_id_sensors_) {
    ts->publish_state(s);
  }
#endif
}

}  // namespace esphome::vitohome
