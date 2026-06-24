#include "vitohome.h"

#include <cinttypes>
#include <cstdio>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace vitohome {

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
  if (this->is_failed()) return;

  // OptolinkEngine<P300> takes the protocol tag only; the constructor deduces
  // the interface type and wraps &iface_ in a GenericInterface internally.
  this->vito_ = std::make_unique<optolink::OptolinkEngine<optolink::P300>>(&this->iface_);

  // VS2 callbacks are std::function (verified at the pinned SHA), so they
  // can capture `this` directly — no static-instance indirection needed.
  this->vito_->onResponse([this](const optolink::PacketVS2 &response, const optolink::Datapoint &request) {
    this->on_response_(response, request);
  });
  this->vito_->onError(
      [this](optolink::OptolinkResult error, const optolink::Datapoint &request) { this->on_error_(error, request); });

  if (!this->vito_->begin()) {
    ESP_LOGE(TAG, "optolink engine begin() failed");
    this->mark_failed();
    return;
  }

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
  if (this->vito_ == nullptr) return;

  this->vito_->loop();

  // Watchdog: if a request has been in flight too long, surface that and
  // free the slot.
  if (this->in_flight_ != nullptr || this->ident_in_flight_) {
    uint32_t now = millis();
    if (now - this->in_flight_started_ms_ > IN_FLIGHT_WATCHDOG_MS) {
      if (this->ident_in_flight_) {
        ESP_LOGW(TAG, "Identification read exceeded watchdog (%" PRIu32 " ms)", IN_FLIGHT_WATCHDOG_MS);
        this->ident_in_flight_ = false;
        this->ident_handle_error_();
      } else {
        ESP_LOGW(TAG, "In-flight %s to %s exceeded watchdog (%" PRIu32 " ms). Clearing.",
                 this->in_flight_op_ == OpType::WRITE ? "write" : "read", this->in_flight_->get_datapoint().name(),
                 IN_FLIGHT_WATCHDOG_MS);
        this->in_flight_->handle_error(optolink::OptolinkResult::TIMEOUT);
        if (this->in_flight_op_ == OpType::READ) {
          this->in_flight_->read_queued_ = false;
        } else {
          this->in_flight_->write_in_flight_ = false;
        }
        this->in_flight_ = nullptr;
        this->in_flight_op_ = OpType::NONE;
      }
    }
  }

  // Dispatch the next queued request if the bus is idle.
  this->dispatch_next_();
}

void VitoHomeComponent::dispatch_next_() {
  if (this->in_flight_ != nullptr || this->ident_in_flight_) return;

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

  // Writes preempt reads: a user-initiated setpoint change should not wait
  // behind a full poll cycle.
  if (!this->write_queue_.empty()) {
    VitoEntityBase *entity = this->write_queue_.front();
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

  if (this->read_queue_.empty()) return;
  VitoEntityBase *entity = this->read_queue_.front();
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
  for (auto *entity : this->entities_) {
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
  if (this->entities_.empty()) return;
  this->schedule_due_entities_();
}

void VitoHomeComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "VitoHome:");
  ESP_LOGCONFIG(TAG, "  Protocol: P300 (VS2)");
  ESP_LOGCONFIG(TAG, "  Entities: %zu", this->entities_.size());
  if (this->ident_state_ == IdentState::DONE) {
    ESP_LOGCONFIG(TAG, "  Device: %s", this->ident_string_().c_str());
  }
  this->check_uart_settings(4800, 2, uart::UART_CONFIG_PARITY_EVEN, 8);
  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Setup FAILED");
    return;
  }
  for (auto *e : this->entities_) {
    e->dump_config();
  }
}

bool VitoHomeComponent::request_write(VitoEntityBase *entity) {
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

void VitoHomeComponent::on_response_(const optolink::PacketVS2 &response, const optolink::Datapoint &request) {
  if (this->ident_in_flight_) {
    this->ident_in_flight_ = false;
    this->ident_handle_response_(response);
    return;
  }

  VitoEntityBase *entity = this->in_flight_;
  OpType op = this->in_flight_op_;
  this->in_flight_ = nullptr;
  this->in_flight_op_ = OpType::NONE;
  if (entity == nullptr) {
    ESP_LOGW(TAG, "Response received for 0x%04X but no in-flight request", request.address());
    return;
  }
  // A write was dispatched to the command address; a read to the state
  // address. Match the response against whichever this op used (they differ
  // only for two-address controls; otherwise both are datapoint_).
  const uint16_t expected_addr =
      (op == OpType::WRITE) ? entity->get_write_datapoint().address() : entity->get_datapoint().address();
  if (expected_addr != request.address()) {
    ESP_LOGW(TAG, "Response address 0x%04X does not match in-flight 0x%04X; dropping", request.address(),
             expected_addr);
    entity->read_queued_ = false;
    // Clear only the in-flight marker; if a newer value re-enqueued during this
    // transaction, write_queued_ stays set so it is still transmitted.
    entity->write_in_flight_ = false;
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

void VitoHomeComponent::on_error_(optolink::OptolinkResult error, const optolink::Datapoint &request) {
  if (this->ident_in_flight_) {
    this->ident_in_flight_ = false;
    ESP_LOGD(TAG, "Identification read 0x%04X len %u failed (%s)", request.address(), request.length(),
             optolink::errorToString(error));
    this->ident_handle_error_();
    return;
  }

  VitoEntityBase *entity = this->in_flight_;
  OpType op = this->in_flight_op_;
  this->in_flight_ = nullptr;
  this->in_flight_op_ = OpType::NONE;

  const char *name = request.name();
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
    } else {
      entity->write_in_flight_ = false;
    }
    entity->handle_error(error);
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

void VitoHomeComponent::ident_handle_response_(const optolink::PacketVS2 &response) {
  const uint8_t *d = response.data();
  const uint8_t n = response.dataLength();
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
    ESP_LOGI(TAG,
             "Software index (0xFB) unavailable — when picking datapoints from the "
             "Vitosoft data, match on the family only and verify on the wire.");
  }
  for (auto *ts : this->device_id_sensors_) {
    ts->publish_state(s);
  }
}

}  // namespace vitohome
}  // namespace esphome
