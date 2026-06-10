#include "vitohome.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace vitohome {

static const char *const TAG = "vitohome";

VitoHomeComponent *VitoHomeComponent::instance_ = nullptr;

void VitoHomeComponent::setup() {
  if (instance_ != nullptr) {
    ESP_LOGE(TAG,
             "Only one VitoHome component is supported per device. "
             "Remove the duplicate vitohome: block.");
    this->mark_failed();
    return;
  }
  instance_ = this;

  this->validate_uart_();
  if (this->is_failed()) return;

  // VitoWiFi<VS2> takes the protocol version only; the constructor deduces
  // the interface type and wraps &iface_ in a GenericInterface internally.
  this->vito_ = std::make_unique<VitoWiFi::VitoWiFi<VitoWiFi::VS2>>(&this->iface_);

  this->vito_->onResponse(&VitoHomeComponent::on_response_);
  this->vito_->onError(&VitoHomeComponent::on_error_);

  if (!this->vito_->begin()) {
    ESP_LOGE(TAG, "VitoWiFi::begin() failed");
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "VitoHome ready, %zu entities registered", this->entities_.size());
}

void VitoHomeComponent::validate_uart_() {
  // The Optolink requires 4800 8E2. Fail loudly here rather than spend an
  // hour debugging silent bus errors. get_baud_rate/get_data_bits/
  // get_stop_bits/get_parity are stable accessors in current ESPHome
  // (uart_component.h); dump_config() additionally emits the standard
  // check_uart_settings log line.
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

  // Watchdog: if a request has been in flight too long, surface that
  // and free the slot.
  if (this->in_flight_ != nullptr) {
    uint32_t now = millis();
    if (now - this->in_flight_started_ms_ > IN_FLIGHT_WATCHDOG_MS) {
      ESP_LOGW(TAG, "In-flight request to %s exceeded watchdog (%u ms). Clearing.",
               this->in_flight_->get_datapoint().name(), IN_FLIGHT_WATCHDOG_MS);
      this->in_flight_->handle_error(VitoWiFi::OptolinkResult::TIMEOUT);
      this->in_flight_ = nullptr;
    }
  }

  // Dispatch the next queued request if the bus is idle.
  this->dispatch_next_();
}

void VitoHomeComponent::dispatch_next_() {
  if (this->in_flight_ != nullptr) return;
  if (this->queue_.empty()) return;

  VitoEntityBase *entity = this->queue_.front();
  if (this->vito_->read(entity->get_datapoint())) {
    this->in_flight_ = entity;
    this->in_flight_started_ms_ = millis();
    this->queue_.pop_front();
    ESP_LOGV(TAG, "Dispatched read for %s", entity->get_datapoint().name());
  }
  // else: VitoWiFi engine is busy with internal state; retry next loop().
}

void VitoHomeComponent::update() {
  if (this->vito_ == nullptr) return;
  if (this->entities_.empty()) return;

  // If the previous cycle hasn't drained, log and skip this tick. With
  // ~10 entities at ~100 ms each the cycle takes ~1 s, which is far
  // below typical update_interval values, so this should be rare.
  if (!this->queue_.empty() || this->in_flight_ != nullptr) {
    ESP_LOGW(TAG, "Skipping poll cycle: %zu still queued, %s in flight", this->queue_.size(),
             this->in_flight_ != nullptr ? "request" : "none");
    return;
  }

  for (auto *entity : this->entities_) {
    this->queue_.push_back(entity);
  }
  ESP_LOGV(TAG, "Queued %zu reads", this->queue_.size());
}

void VitoHomeComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "VitoHome:");
  ESP_LOGCONFIG(TAG, "  Protocol: P300 (VS2)");
  ESP_LOGCONFIG(TAG, "  Entities: %zu", this->entities_.size());
  this->check_uart_settings(4800, 2, uart::UART_CONFIG_PARITY_EVEN, 8);
  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Setup FAILED");
    return;
  }
  for (auto *e : this->entities_) {
    e->dump_config();
  }
}

void VitoHomeComponent::on_response_(const VitoWiFi::PacketVS2 &response, const VitoWiFi::Datapoint &request) {
  if (instance_ == nullptr) return;
  VitoEntityBase *entity = instance_->in_flight_;
  instance_->in_flight_ = nullptr;
  if (entity == nullptr) {
    ESP_LOGW(TAG, "Response received for 0x%04X but no in-flight request", request.address());
    return;
  }
  if (entity->get_datapoint().address() != request.address()) {
    ESP_LOGW(TAG, "Response address 0x%04X does not match in-flight 0x%04X; dropping", request.address(),
             entity->get_datapoint().address());
    return;
  }
  entity->handle_response(response);
}

void VitoHomeComponent::on_error_(VitoWiFi::OptolinkResult error, const VitoWiFi::Datapoint &request) {
  if (instance_ == nullptr) return;
  VitoEntityBase *entity = instance_->in_flight_;
  instance_->in_flight_ = nullptr;

  const char *name = request.name();
  switch (error) {
    case VitoWiFi::OptolinkResult::TIMEOUT:
      ESP_LOGE(TAG, "[TIMEOUT] %s — Optolink not responding", name);
      break;
    case VitoWiFi::OptolinkResult::LENGTH:
      ESP_LOGE(TAG, "[LENGTH]  %s — invalid payload length", name);
      break;
    case VitoWiFi::OptolinkResult::NACK:
      ESP_LOGW(TAG,
               "[NACK]    %s — heater rejected request "
               "(unsupported address?)",
               name);
      break;
    case VitoWiFi::OptolinkResult::CRC:
      ESP_LOGE(TAG, "[CRC]     %s — checksum mismatch (wiring?)", name);
      break;
    case VitoWiFi::OptolinkResult::ERROR:
    default:
      ESP_LOGE(TAG, "[ERROR]   %s — protocol error", name);
      break;
  }
  if (entity != nullptr) entity->handle_error(error);
}

}  // namespace vitohome
}  // namespace esphome
