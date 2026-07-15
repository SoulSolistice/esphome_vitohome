#include "vitohome.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <limits>

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

  // Size the run-loop queues once, now that registration is complete. Each
  // lane makes exactly one element-storage allocation here and never
  // reallocates:
  //
  //   * read/write lanes: registered entity count;
  //   * raw lane: configured scan-sweep cap.
  //
  // All failures are handled at boot rather than leaving a partially working
  // component. Separate checks identify the allocation that failed.
  const std::size_t entity_count = this->entities_.size();

  if (!this->read_queue_.reserve(entity_count)) {
    ESP_LOGE(TAG, "failed to allocate read queue for %zu entities", entity_count);
    this->mark_failed();
    return;
  }

  if (!this->write_queue_.reserve(entity_count)) {
    ESP_LOGE(TAG, "failed to allocate write queue for %zu entities", entity_count);
    this->mark_failed();
    return;
  }

  if (!this->raw_queue_.reserve(RAW_QUEUE_MAX)) {
    ESP_LOGE(TAG, "failed to allocate raw queue with capacity %zu", RAW_QUEUE_MAX);
    this->mark_failed();
    return;
  }

  // The engine is build-time-selected (protocol_select.h) and deduces the
  // interface type, wrapping &iface_ in a GenericInterface internally.
  this->vito_ = std::make_unique<optolink::OptolinkEngine<SelectedProtocol>>(&this->iface_);

  // All three engines share one byte-mover callback shape, so the hub registers
  // directly with the engine and wraps the raw payload in a ResponseView here.
  //
  // On P300 `address` is the one echoed in the device's own response frame (a
  // real wire-level datum); on KW/GWG the engine echoes the retained request
  // address. The engine is strictly single-in-flight and the hub tracks its own
  // in-flight context (in_flight_ / ident_in_flight_ / raw_in_flight_), so the
  // callback carries only that address as a wire-level cross-check.
  this->vito_->onResponse([this](const uint8_t *data, uint8_t length, uint16_t address) {
    // A complete valid response proves link liveness and validates the
    // configured protocol for start-up verification.
    this->link_note_alive_();
    this->on_response_(ResponseView{data, length, address}, address);
  });

  this->vito_->onError([this](optolink::OptolinkResult error, uint16_t request_address) {
    // Link health tracks a persistent no-response condition rather than every
    // failed operation:
    //
    //   * NACK means the device received the request and actively rejected it.
    //   * DEVICE_ERROR is a COMPLETE, checksum-valid error frame from the
    //     device -- proof the peer answered in this protocol.
    //   * TIMEOUT means no usable reply arrived within the engine watchdog.
    //   * ERROR is malformed traffic (an invalid frame after a start byte),
    //     possibly line noise; CRC/LENGTH likewise indicate corruption. None
    //     of these establishes either a healthy response or a silent link.
    //
    // NACK and DEVICE_ERROR therefore reset the timeout streak (and satisfy
    // start-up protocol verification), while only TIMEOUT advances it.
    switch (error) {
      case optolink::OptolinkResult::NACK:
      case optolink::OptolinkResult::DEVICE_ERROR:
        this->link_note_alive_();
        break;

      case optolink::OptolinkResult::TIMEOUT:
        this->link_note_error_();
        break;

      case optolink::OptolinkResult::ERROR:
      case optolink::OptolinkResult::CRC:
      case optolink::OptolinkResult::LENGTH:
      default:
        break;
    }

    this->on_error_(error, request_address);
  });

  if (!this->vito_->begin()) {
    ESP_LOGE(TAG, "optolink engine begin() failed");
    this->mark_failed();
    return;
  }

  // Require the configured protocol to establish a link within the start-up
  // window: max(PROTOCOL_VERIFY_MIN_MS, 3 * hub update interval).
  //
  // The deadline comparison in loop() uses a signed uint32_t difference and is
  // therefore valid only for deadlines less than 2^31 milliseconds away.
  // Saturate the derived window at INT32_MAX instead of allowing either the
  // multiplication or the signed-difference assumption to overflow.
  const uint32_t interval = this->get_update_interval();
  constexpr uint32_t MAX_SIGNED_DEADLINE_MS = static_cast<uint32_t>(std::numeric_limits<int32_t>::max());

  uint32_t verify_window;
  if (interval > MAX_SIGNED_DEADLINE_MS / 3u) {
    verify_window = MAX_SIGNED_DEADLINE_MS;
  } else {
    verify_window = interval * 3u;
  }

  if (verify_window < PROTOCOL_VERIFY_MIN_MS)
    verify_window = PROTOCOL_VERIFY_MIN_MS;

  this->protocol_verify_pending_ = true;

  // A one-shot setup anchor, not a hot-loop read.
  // App.get_loop_component_start_time() exists to avoid repeated slow millis()
  // reads on hot paths; that benefit does not apply to one setup-time deadline.
  this->protocol_verify_deadline_ms_ = millis() + verify_window;

  // Per-entity intervals are scheduled at hub-tick granularity, so anything
  // shorter than the hub interval silently degrades to the hub interval.
  // Surface that at setup instead of letting the user chase phantom lag.
  const uint32_t hub_interval = this->get_update_interval();
  for (auto *entity : this->entities_) {
    if (entity == nullptr)
      continue;

    if (entity->poll_interval() != 0 && entity->poll_interval() < hub_interval) {
      ESP_LOGW(TAG,
               "%s '%s': update_interval %" PRIu32 " ms is shorter than the hub's %" PRIu32
               " ms; effective rate is the hub interval",
               entity->entity_kind(), entity->get_datapoint().name(), entity->poll_interval(), hub_interval);
    }
  }

  if (this->identify_device_)
    this->ident_start_();

  ESP_LOGI(TAG, "VitoHome ready, %zu entities registered", this->entities_.size());
}

void VitoHomeComponent::validate_uart_() {
  // The Optolink requires 4800 8E2. Fail loudly here rather than spend an
  // hour debugging silent bus errors.
  auto *bus = this->parent_;
  if (bus == nullptr) {
    ESP_LOGE(TAG, "UART parent is not configured");
    this->mark_failed();
    return;
  }

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
  if (this->vito_ == nullptr || this->is_failed())
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
      // Rollover-safe signed-difference comparison. setup() caps the deadline
      // distance at INT32_MAX so this comparison remains valid.
      this->protocol_verify_pending_ = false;

      ESP_LOGE(TAG, "%s link not established; check wiring and that the device speaks this protocol", PROTOCOL_NAME);

      this->publish_link_(false);
      this->mark_failed();

      // Do not continue into watchdog handling or dispatch after failure.
      return;
    }
  }

  // Watchdog: if a request has been in flight too long, surface that and free
  // the hub slot. The protocol engines have their own shorter timeout, so this
  // is a last-resort guard for a lost callback rather than the normal timeout
  // mechanism.
  if (this->in_flight_ != nullptr || this->ident_in_flight_ || this->raw_in_flight_) {
    const uint32_t now = App.get_loop_component_start_time();

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
        // in_flight_ is known non-null here because the identification and raw
        // branches were excluded.
        ESP_LOGW(TAG, "In-flight %s to %s exceeded watchdog (%" PRIu32 " ms). Clearing.",
                 this->in_flight_op_ == OpType::WRITE ? "write" : "read", this->in_flight_->get_datapoint().name(),
                 IN_FLIGHT_WATCHDOG_MS);

        VitoEntityBase *entity = this->in_flight_;
        const OpType operation = this->in_flight_op_;

        this->in_flight_ = nullptr;
        this->in_flight_op_ = OpType::NONE;

        if (operation == OpType::READ) {
          entity->read_queued_ = false;
          entity->handle_error(optolink::OptolinkResult::TIMEOUT);
        } else if (operation == OpType::WRITE) {
          entity->write_in_flight_ = false;
          entity->handle_write_error(optolink::OptolinkResult::TIMEOUT);
        } else {
          ESP_LOGE(TAG, "watchdog found entity in flight with no operation type");
        }
      }
    }
  }

  // Dispatch the next queued request if the bus is idle.
  this->dispatch_next_();
}

void VitoHomeComponent::dispatch_raw_front_() {
  // Keep the front item stable across the engine hand-off and remove it only
  // if read()/write() accepts it. The engine copies a write payload into its
  // own fixed packet synchronously inside write().
  this->raw_queue_.consume_front_if([this](const RawOp &operation) {
    this->raw_dp_ = optolink::Datapoint(operation.purpose == RawPurpose::SCAN ? "scan" : "clock", operation.address,
                                        operation.length, optolink::noconv);

    this->raw_is_write_ = operation.is_write;
    this->raw_purpose_ = operation.purpose;

    bool dispatched;
    if (operation.is_write) {
      dispatched = this->vito_->write(this->raw_dp_.address(), operation.bytes, operation.bytes_len);
    } else {
      dispatched = this->vito_->read(this->raw_dp_.address(), this->raw_dp_.length());
    }

    if (!dispatched)
      return false;

    this->raw_write_len_ = operation.is_write ? operation.bytes_len : 0;
    this->raw_in_flight_ = true;
    this->in_flight_started_ms_ = App.get_loop_component_start_time();

    ESP_LOGV(TAG, "Dispatched raw %s 0x%04X len %u", operation.is_write ? "write" : "read", operation.address,
             operation.length);

    return true;
  });
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

  // Interactive scan-console operations preempt regular polling and queued user
  // writes only when a SCAN item is currently at the front of the shared raw
  // FIFO. A CLOCK_* item already ahead of it retains FIFO order.
  RawOp raw_front{};
  if (this->raw_queue_.try_front(raw_front) && raw_front.purpose == RawPurpose::SCAN) {
    this->dispatch_raw_front_();
    return;
  }

  // Writes preempt reads and background clock-sync operations. The ring keeps
  // the entity stable at the front until the engine accepts it.
  const auto write_result = this->write_queue_.consume_front_if([this](VitoEntityBase *entity) {
    if (entity == nullptr) {
      // A null entry violates registration/enqueue invariants. Remove it so it
      // cannot block all later writes.
      ESP_LOGE(TAG, "null entity in write queue; dropping item");
      return true;
    }

    if (!this->vito_->write(entity->get_write_datapoint().address(), entity->write_data(), entity->write_length())) {
      return false;
    }

    this->in_flight_ = entity;
    this->in_flight_op_ = OpType::WRITE;
    this->in_flight_started_ms_ = App.get_loop_component_start_time();

    // The entity has left the queue and is now in flight. Clearing
    // write_queued_ here lets a newer value enqueue while this request waits
    // for its ACK.
    entity->write_queued_ = false;
    entity->write_in_flight_ = true;

    ESP_LOGV(TAG, "Dispatched write for %s (%u bytes)", entity->get_datapoint().name(), entity->write_length());

    return true;
  });

  if (write_result != RingBuffer<VitoEntityBase *>::ConsumeResult::EMPTY)
    return;

  // Background clock synchronization gets a turn once no user write is
  // pending. The front may also be a SCAN item if another producer appended or
  // otherwise changed queue state after the earlier snapshot; dispatching it
  // here remains valid.
  if (!this->raw_queue_.empty()) {
    this->dispatch_raw_front_();
    return;
  }

  // Poll/read-back lane. read_queued_ remains true after the item leaves the
  // ring and while its request is in flight. Completion, error, mismatch, or
  // watchdog handling clears it.
  this->read_queue_.consume_front_if([this](VitoEntityBase *entity) {
    if (entity == nullptr) {
      ESP_LOGE(TAG, "null entity in read queue; dropping item");
      return true;
    }

    const optolink::Datapoint &datapoint = entity->get_datapoint();

    if (!this->vito_->read(datapoint.address(), datapoint.length()))
      return false;

    this->in_flight_ = entity;
    this->in_flight_op_ = OpType::READ;
    this->in_flight_started_ms_ = App.get_loop_component_start_time();

    ESP_LOGV(TAG, "Dispatched read for %s", entity->get_datapoint().name());

    return true;
  });
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
  //
  //  2. Even when it did fire, the period crept: each cycle added the
  //     accumulated jitter into the next due time.
  //
  // SLACK absorbs sub-tick jitter in the "is it due yet" test: anything due
  // within half a hub tick of now is treated as due now, because the next
  // opportunity to poll it is a full hub tick away and firing a few ms early
  // beats firing a whole tick late.
  const uint32_t slack = this->get_update_interval() / 2;

  std::size_t queued = 0;
  std::size_t skipped = 0;
  std::size_t rejected = 0;

  for (auto *entity : this->entities_) {
    if (entity == nullptr)
      continue;

    if (entity->read_queued_) {
      ++skipped;
      continue;
    }

    const PollDecision decision = poll_schedule_step(now, entity->next_due_ms_, entity->poll_interval(), slack);

    if (!decision.due)
      continue;

    // Set the companion state before publishing the queue item. If the bounded
    // lane rejects it, roll the state back immediately so the entity is not
    // permanently wedged as "queued" while absent from the queue.
    entity->read_queued_ = true;

    if (!this->read_queue_.push_back(entity)) {
      entity->read_queued_ = false;
      ++rejected;
      continue;
    }

    // Advance the schedule only after insertion succeeded. Otherwise a
    // rejected push would suppress retry for a complete poll interval.
    if (entity->poll_interval() != 0)
      entity->next_due_ms_ = decision.next_due_ms;

    ++queued;
  }

  if (skipped != 0) {
    ESP_LOGW(TAG, "Poll cycle: %zu entities still queued from the previous cycle (bus saturated?)", skipped);
  }

  if (rejected != 0) {
    ESP_LOGE(TAG, "Poll cycle: read queue rejected %zu due entities (size=%zu, capacity=%zu)", rejected,
             this->read_queue_.size(), this->read_queue_.capacity());
  }

  ESP_LOGV(TAG, "Queued %zu reads", queued);
}

void VitoHomeComponent::update() {
  if (this->vito_ == nullptr || this->is_failed())
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

  if (this->ident_state_ == IdentState::DONE)
    ESP_LOGCONFIG(TAG, "  Device: %s", this->ident_string_().c_str());

#ifdef USE_TEXT_SENSOR
  if (!this->raw_result_sensors_.empty()) {
    ESP_LOGCONFIG(TAG, "  Scan console: %zu scan_result sensor(s) attached", this->raw_result_sensors_.size());
  }
#endif

  if (ESPHomeUARTInterface::frame_logging_enabled())
    ESP_LOGCONFIG(TAG, "  Frame logging: ON (tag 'vitohome.frames')");

  this->check_uart_settings(4800, 2, uart::UART_CONFIG_PARITY_EVEN, 8);

  if (this->is_failed())
    ESP_LOGE(TAG, "  Setup FAILED");

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
    // dispatch will transmit the latest value. Coalesce -- nothing to add.
    return true;
  }

  // Not queued. Either idle, or a write for this entity is currently in flight.
  // In the in-flight case the old bytes were already copied into the engine
  // packet, so the entity staging buffer now contains a newer value that must be
  // queued for a later transaction.
  //
  // Set the logical state before publishing the queue entry, then roll it back
  // if the bounded insertion is rejected.
  entity->write_queued_ = true;

  if (!this->write_queue_.push_back(entity)) {
    entity->write_queued_ = false;

    ESP_LOGE(TAG, "write queue full (%zu/%zu); write for %s was not queued", this->write_queue_.size(),
             this->write_queue_.capacity(), entity->get_datapoint().name());

    return false;
  }

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
  // The 32-byte cap also keeps packet-length arithmetic safe: the VS2 length
  // byte is 0x05 + len and the VS1 frame length is payload + 4. See the packet
  // implementations before raising this cap.
  if (bytes.empty() || bytes.size() > RAW_WRITE_MAX) {
    ESP_LOGW(TAG, "queue_raw_write: %zu bytes out of range (1..%u)", bytes.size(), RAW_WRITE_MAX);
    return;
  }

  this->enqueue_raw_(address, static_cast<uint8_t>(bytes.size()), true, bytes.data(),
                     static_cast<uint8_t>(bytes.size()), RawPurpose::SCAN);
}

bool VitoHomeComponent::enqueue_raw_(uint16_t address, uint8_t length, bool is_write, const uint8_t *bytes,
                                     uint8_t bytes_len, RawPurpose purpose) {
  if (length == 0) {
    ESP_LOGW(TAG, "enqueue_raw_: zero-length operation rejected");
    return false;
  }

  if (bytes_len > RAW_WRITE_MAX) {
    ESP_LOGW(TAG, "enqueue_raw_: %u bytes exceeds %u; dropping", bytes_len, RAW_WRITE_MAX);
    return false;
  }

  if (is_write) {
    if (bytes == nullptr || bytes_len == 0 || bytes_len != length) {
      ESP_LOGW(TAG, "enqueue_raw_: invalid write payload (length=%u, bytes_len=%u, data=%s)", length, bytes_len,
               bytes == nullptr ? "null" : "set");
      return false;
    }
  } else if (bytes != nullptr || bytes_len != 0) {
    ESP_LOGW(TAG, "enqueue_raw_: read operation carried a payload");
    return false;
  }

  RawOp operation{};
  operation.address = address;
  operation.length = length;
  operation.is_write = is_write;
  operation.bytes_len = bytes_len;
  operation.purpose = purpose;

  if (is_write)
    std::memcpy(operation.bytes, bytes, bytes_len);

  // push_back() is authoritative. A preceding full() check would be a
  // check/use race when multiple producers are possible.
  if (!this->raw_queue_.push_back(operation)) {
    ESP_LOGW(TAG, "raw queue full (%zu/%zu); dropping %s 0x%04X", this->raw_queue_.size(), this->raw_queue_.capacity(),
             is_write ? "write" : "read", address);
    return false;
  }

  ESP_LOGD(TAG, "Queued raw %s 0x%04X len %u", is_write ? "write" : "read", address, length);
  return true;
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
  // 1..8 + NUL. 208 covers a 48-byte dump with room to spare.
  char buffer[208];

  if (this->raw_is_write_) {
    snprintf(buffer, sizeof(buffer), "0x%04X: write ACK (%u byte%s)", this->raw_dp_.address(), this->raw_write_len_,
             this->raw_write_len_ == 1 ? "" : "s");
  } else {
    format_raw_dump(response.address, response.data, response.data_length, buffer, sizeof(buffer));
  }

  ESP_LOGI(TAG, "Raw: %s", buffer);
  this->raw_publish_(buffer);
}

void VitoHomeComponent::raw_handle_error_(optolink::OptolinkResult error) {
  if (this->raw_purpose_ != RawPurpose::SCAN) {
    ESP_LOGW(TAG, "System-time sync: %s 0x%04X failed (%s)", this->raw_is_write_ ? "write" : "read",
             this->raw_dp_.address(), optolink::errorToString(error));
    return;
  }

  char buffer[96];

  snprintf(buffer, sizeof(buffer), "0x%04X: %s FAILED (%s)", this->raw_dp_.address(),
           this->raw_is_write_ ? "write" : "read", optolink::errorToString(error));

  ESP_LOGW(TAG, "Raw: %s", buffer);
  this->raw_publish_(buffer);
}

void VitoHomeComponent::raw_publish_(const std::string &line) {
#ifdef USE_TEXT_SENSOR
  for (auto *sensor : this->raw_result_sensors_) {
    if (sensor != nullptr)
      sensor->publish_state(line);
  }
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
  if (!this->enqueue_raw_(CLOCK_ADDRESS, CLOCK_LEN, false, nullptr, 0, RawPurpose::CLOCK_READ)) {
    ESP_LOGW(TAG, "System-time sync: clock read could not be queued");
  }
#endif
}

void VitoHomeComponent::clock_handle_read_(const ResponseView &response) {
#ifdef VITOHOME_TIME_SYNC
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

    if (magnitude <= static_cast<int64_t>(this->time_drift_threshold_s_)) {
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

  uint8_t buffer[CLOCK_LEN];
  const uint8_t weekday = device_weekday_from_esptime(time.day_of_week);

  if (!encode_datetime_bcd(time.year, time.month, time.day_of_month, weekday, time.hour, time.minute, time.second,
                           buffer)) {
    ESP_LOGW(TAG, "System-time sync: time source out of range, skipping");
    return;
  }

  if (!this->enqueue_raw_(CLOCK_ADDRESS, CLOCK_LEN, true, buffer, CLOCK_LEN, RawPurpose::CLOCK_WRITE)) {
    ESP_LOGW(TAG, "System-time sync: clock write could not be queued");
  }
#else
  (void) response;
#endif
}

void VitoHomeComponent::clock_handle_write_ack_() {
  ESP_LOGI(TAG, "System-time sync: device clock set; reading back to confirm");

  if (!this->enqueue_raw_(CLOCK_ADDRESS, CLOCK_LEN, false, nullptr, 0, RawPurpose::CLOCK_VERIFY)) {
    ESP_LOGW(TAG, "System-time sync: verification read could not be queued");
  }
}

void VitoHomeComponent::clock_handle_verify_(const ResponseView &response) {
  BcdDateTime device_time{};

  if (decode_datetime_bcd(response.data, response.data_length, 0, &device_time)) {
    ESP_LOGI(TAG, "System-time sync: device clock now %04u-%02u-%02u %02u:%02u:%02u", device_time.year,
             device_time.month, device_time.day, device_time.hour, device_time.minute, device_time.second);
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

    // P300 carries a real response address. KW/GWG echo the request address, so
    // the same check is harmless there. Reject a mismatched raw response rather
    // than publishing or feeding it into the clock state machine.
    if (response.address != this->raw_dp_.address()) {
      ESP_LOGW(TAG, "Raw response address 0x%04X does not match in-flight 0x%04X; dropping", response.address,
               this->raw_dp_.address());
      this->raw_handle_error_(optolink::OptolinkResult::ERROR);
      return;
    }

    this->raw_handle_response_(response);
    return;
  }

  VitoEntityBase *entity = this->in_flight_;
  const OpType operation = this->in_flight_op_;

  this->in_flight_ = nullptr;
  this->in_flight_op_ = OpType::NONE;

  if (entity == nullptr) {
    ESP_LOGW(TAG, "Response received for 0x%04X but no in-flight request", request_address);
    return;
  }

  // A write was dispatched to the command address; a read to the state
  // address. Match the response against whichever this operation used.
  //
  // On P300 response.address is the address echoed in the device's own frame,
  // so this is a live wire-level check. KW/GWG carry no response address and
  // echo the retained request address, making the check tautological there.
  const uint16_t expected_address =
      operation == OpType::WRITE ? entity->get_write_datapoint().address() : entity->get_datapoint().address();

  if (expected_address != response.address) {
    ESP_LOGW(TAG, "Response address 0x%04X does not match in-flight 0x%04X; dropping", response.address,
             expected_address);

    // Clear only the state belonging to this operation. If a newer write was
    // queued while an older one was in flight, write_queued_ remains set.
    if (operation == OpType::READ) {
      entity->read_queued_ = false;
    } else if (operation == OpType::WRITE) {
      entity->write_in_flight_ = false;
    }

    return;
  }

  if (operation == OpType::WRITE) {
    entity->write_in_flight_ = false;

    ESP_LOGD(TAG, "Write to %s acknowledged", entity->get_datapoint().name());

    // If control() queued a newer value while this request was in flight, this
    // ACK belongs to the older payload. The entity's pending_* member and write
    // buffer already describe the NEWER value.
    //
    // Calling handle_write_response() here could therefore publish a value the
    // device has not accepted yet when read_back is disabled. Likewise, an
    // immediate read-back would observe the intermediate device state before
    // the newer queued write. Let the newer write retain priority; its own ACK
    // will perform publication or read-back.
    if (entity->write_queued_) {
      ESP_LOGV(TAG, "Write ACK for %s superseded by a newer queued value", entity->get_datapoint().name());
      return;
    }

    entity->handle_write_response(response);

    if (entity->wants_read_back() && !entity->read_queued_) {
      // Confirm by reading the device's view of the value, ahead of the regular
      // poll queue.
      entity->read_queued_ = true;

      if (!this->read_queue_.push_front(entity)) {
        entity->read_queued_ = false;

        ESP_LOGE(TAG, "read queue full (%zu/%zu); immediate read-back for %s was not queued", this->read_queue_.size(),
                 this->read_queue_.capacity(), entity->get_datapoint().name());
      }
    }

    return;
  }

  if (operation == OpType::READ) {
    entity->read_queued_ = false;
    entity->handle_response(response);
    return;
  }

  ESP_LOGW(TAG, "Response received for %s with no valid operation type", entity->get_datapoint().name());
}

bool VitoHomeComponent::refresh_all() {
  // Fresh millis() is deliberate here (not
  // App.get_loop_component_start_time()). refresh_all() is a rare cold path
  // entered from a button press or user lambda, potentially from another
  // component's loop dispatch rather than the hub's own callback.
  const uint32_t now = millis();

  if (this->last_refresh_all_ms_ != 0 && now - this->last_refresh_all_ms_ < REFRESH_ALL_MIN_INTERVAL_MS) {
    ESP_LOGW(TAG, "refresh_all() suppressed (last one %" PRIu32 " ms ago, min interval %" PRIu32 " ms)",
             now - this->last_refresh_all_ms_, REFRESH_ALL_MIN_INTERVAL_MS);
    return false;
  }

  this->last_refresh_all_ms_ = now;

  for (auto *entity : this->entities_) {
    if (entity != nullptr)
      entity->next_due_ms_ = 0;
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
  for (auto *sensor : this->link_sensors_) {
    if (sensor != nullptr)
      sensor->publish_state(up);
  }
#endif
}

void VitoHomeComponent::link_note_alive_() {
  this->link_established_ = true;
  this->link_error_streak_ = 0;
  this->publish_link_(true);
}

void VitoHomeComponent::link_note_error_() {
  if (this->link_error_streak_ < LINK_OFFLINE_AFTER_ERRORS)
    ++this->link_error_streak_;

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
  const OpType operation = this->in_flight_op_;

  this->in_flight_ = nullptr;
  this->in_flight_op_ = OpType::NONE;

  // Name for the log line: the in-flight entity's datapoint when there is one,
  // else the echoed request address for a stray callback.
  const char *name = entity != nullptr ? entity->get_datapoint().name() : "?";

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

    case optolink::OptolinkResult::DEVICE_ERROR:
      ESP_LOGW(TAG, "[DEVERR]  %s (0x%04X) — device returned an error frame (unsupported address or length?)", name,
               request_address);
      break;

    case optolink::OptolinkResult::ERROR:
    default:
      ESP_LOGE(TAG, "[ERROR]   %s (0x%04X) — protocol error", name, request_address);
      break;
  }

  if (entity == nullptr)
    return;

  if (operation == OpType::READ) {
    entity->read_queued_ = false;
    entity->handle_error(error);
  } else if (operation == OpType::WRITE) {
    entity->write_in_flight_ = false;

    // The device value did not change on a failed write. The entity keeps its
    // published state by default rather than going unavailable. A newer write
    // that was queued during this transaction remains queued.
    entity->handle_write_error(error);
  } else {
    ESP_LOGW(TAG, "Error callback for %s had no valid operation type", name);
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
  // P300 carries the actual response address. KW/GWG echo the request address,
  // so this check is harmless for those protocols.
  if (response.address != this->ident_dp_.address()) {
    ESP_LOGW(TAG, "Identification response address 0x%04X does not match in-flight 0x%04X", response.address,
             this->ident_dp_.address());
    this->ident_handle_error_();
    return;
  }

  const uint8_t *data = response.data;
  const uint8_t length = response.data_length;

  // A non-zero payload length must have usable storage. Treat a malformed view
  // as a failed identification step rather than dereferencing nullptr.
  if (length != 0 && data == nullptr) {
    ESP_LOGW(TAG, "Identification response has length %u but no payload", length);
    this->ident_handle_error_();
    return;
  }

  switch (this->ident_state_) {
    case IdentState::READ4:
      // 0xF8..0xFB in one transaction. Wire order is register order:
      // F8 = group, F9 = controller, FA = HW index, FB = SW index.
      if (length >= 4) {
        this->ident_group_ = data[0];
        this->ident_controller_ = data[1];
        this->ident_hw_ = data[2];
        this->ident_sw_ = data[3];
        this->ident_finish_();
        return;
      }

      // Short response: fall back to single-byte reads.
      this->ident_dispatch_(IdentState::READ_F8);
      return;

    case IdentState::READ_F8:
      if (length >= 1)
        this->ident_group_ = data[0];

      this->ident_dispatch_(IdentState::READ_F9);
      return;

    case IdentState::READ_F9:
      if (length >= 1)
        this->ident_controller_ = data[0];

      this->ident_dispatch_(IdentState::READ_FA);
      return;

    case IdentState::READ_FA:
      if (length >= 1)
        this->ident_hw_ = data[0];

      this->ident_dispatch_(IdentState::READ_FB);
      return;

    case IdentState::READ_FB:
      if (length >= 1)
        this->ident_sw_ = data[0];

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
  char buffer[96];

  if (this->ident_group_ >= 0 && this->ident_controller_ >= 0) {
    const uint16_t ident = static_cast<uint16_t>((this->ident_group_ << 8) | this->ident_controller_);

    const char *family = ident_family_name(ident);

    const int offset = snprintf(buffer, sizeof(buffer), "0x%04X%s%s%s", ident, family != nullptr ? " (" : "",
                                family != nullptr ? family : "", family != nullptr ? ")" : "");

    if (this->ident_hw_ >= 0 && this->ident_sw_ >= 0 && offset > 0 && offset < static_cast<int>(sizeof(buffer))) {
      snprintf(buffer + offset, sizeof(buffer) - offset, " HW=0x%02X SW=0x%02X", this->ident_hw_, this->ident_sw_);
    }

    return std::string(buffer);
  }

  return std::string("unknown (identification reads failed)");
}

void VitoHomeComponent::ident_finish_() {
  this->ident_state_ = IdentState::DONE;

  const std::string identification = this->ident_string_();

  ESP_LOGI(TAG, "Device identification: %s", identification.c_str());

  if (this->ident_sw_ < 0 && this->ident_group_ >= 0) {
    ESP_LOGI(TAG, "Software index (0xFB) unavailable — when picking datapoints from the "
                  "Vitosoft data, match on the family only and verify on the wire.");
  }

#ifdef USE_TEXT_SENSOR
  for (auto *sensor : this->device_id_sensors_) {
    if (sensor != nullptr)
      sensor->publish_state(identification);
  }
#endif
}

}  // namespace esphome::vitohome
