#include "vito_climate.h"

#include <cmath>

#include "esphome/core/log.h"
#include "vitohome.h"

namespace esphome {
namespace vitohome {

static const char *const TAG = "vitohome.climate";

// --- channel ---------------------------------------------------------------

bool VitoClimateChannel::write_byte(uint8_t value) {
  if (!this->set_write_payload_(&value, 1)) return false;
  return this->vh_parent_ != nullptr && this->vh_parent_->request_write(this);
}

void VitoClimateChannel::handle_response(const ResponseView &response) {
  if (this->kind_ == SETPOINT) {
    this->parent_->on_setpoint_read(response);
  } else {
    this->parent_->on_mode_read(response);
  }
}

// --- channel wiring (needs the complete hub type) --------------------------

void VitoClimate::configure_setpoint(VitoHomeComponent *hub, const optolink::Datapoint &dp, uint32_t poll_ms) {
  this->setpoint_.set_vitohome_parent(hub);
  this->setpoint_.set_datapoint(dp);
  this->setpoint_.set_poll_interval(poll_ms);
  hub->register_entity(&this->setpoint_);
}

void VitoClimate::configure_mode(VitoHomeComponent *hub, const optolink::Datapoint &read_dp, bool read_back,
                                 uint32_t poll_ms) {
  this->has_mode_ = true;
  this->mode_.set_vitohome_parent(hub);
  this->mode_.set_datapoint(read_dp);
  this->mode_.set_read_back(read_back);
  this->mode_.set_poll_interval(poll_ms);
  hub->register_entity(&this->mode_);
}

// --- climate ---------------------------------------------------------------

void VitoClimate::setup() {
  // Register the configured preset names as custom presets so set_custom_preset_
  // accepts them. The const char* point into presets_ (a stable member), which
  // is filled once from codegen and never reallocated after setup.
  if (this->has_mode_ && !this->presets_.empty()) {
    std::vector<const char *> names;
    names.reserve(this->presets_.size());
    for (auto &p : this->presets_) names.push_back(p.name.c_str());
    this->set_supported_custom_presets(names);
  }
  this->mode = this->has_mode_ ? climate::CLIMATE_MODE_OFF : climate::CLIMATE_MODE_HEAT;
}

climate::ClimateTraits VitoClimate::traits() {
  climate::ClimateTraits t;
  // No room sensor: the boiler controls weather-compensated, so the card is
  // target-only. Current-temperature support is a feature flag that we simply
  // do not add (feature_flags_ defaults to 0 = unsupported).
  // OFF is in the mask by default; a heating proxy always offers HEAT, plus
  // whatever modes the configured presets derive.
  t.add_supported_mode(climate::CLIMATE_MODE_HEAT);
  if (this->has_mode_) {
    for (auto &p : this->presets_) t.add_supported_mode(p.mode);
  }
  return t;
}

const VitoClimatePreset *VitoClimate::find_preset_by_name_(const char *name) const {
  for (auto &p : this->presets_) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

const VitoClimatePreset *VitoClimate::first_preset_with_mode_(climate::ClimateMode mode) const {
  for (auto &p : this->presets_) {
    if (p.mode == mode) return &p;
  }
  return nullptr;
}

void VitoClimate::control(const climate::ClimateCall &call) {
  bool changed = false;

  // Target temperature -> room setpoint (1-byte integer degC). Clamp to the
  // configured range, then write; the read-back publishes the device's view.
  if (call.get_target_temperature().has_value()) {
    float t = *call.get_target_temperature();
    if (t < this->setpoint_min_) t = this->setpoint_min_;
    if (t > this->setpoint_max_) t = this->setpoint_max_;
    const uint8_t byte = static_cast<uint8_t>(lroundf(t));
    if (this->setpoint_.write_byte(byte)) {
      this->target_temperature = t;  // optimistic; read-back reconciles
      changed = true;
    } else {
      ESP_LOGE(TAG, "%s: setpoint write could not be queued", this->get_name().c_str());
    }
  }

  // Preset is authoritative for Betriebsart; a mode tap falls back to the first
  // preset that derives that mode (list order is the lever).
  if (this->has_mode_) {
    const VitoClimatePreset *p = nullptr;
    if (call.has_custom_preset()) {
      p = this->find_preset_by_name_(call.get_custom_preset().c_str());
    } else if (call.get_mode().has_value()) {
      p = this->first_preset_with_mode_(*call.get_mode());
    }
    if (p != nullptr) {
      if (this->mode_.write_byte(p->write_value)) {
        this->set_custom_preset_(p->name.c_str());
        this->mode = p->mode;  // optimistic; read-back reconciles
        changed = true;
      } else {
        ESP_LOGE(TAG, "%s: mode write could not be queued", this->get_name().c_str());
      }
    }
  }

  if (changed) this->publish_state();
}

void VitoClimate::on_setpoint_read(const ResponseView &response) {
  if (response.data_length < 1) {
    ESP_LOGW(TAG, "%s: setpoint response too short", this->get_name().c_str());
    return;
  }
  this->target_temperature = static_cast<float>(response.data[0]);
  ESP_LOGD(TAG, "%s setpoint = %u degC", this->get_name().c_str(), response.data[0]);
  this->publish_state();
}

void VitoClimate::on_mode_read(const ResponseView &response) {
  if (response.data_length < 1) {
    ESP_LOGW(TAG, "%s: mode response too short", this->get_name().c_str());
    return;
  }
  const uint8_t byte = response.data[0];
  for (auto &p : this->presets_) {
    for (uint8_t rv : p.read_values) {
      if (rv == byte) {
        this->set_custom_preset_(p.name.c_str());
        this->mode = p.mode;
        ESP_LOGD(TAG, "%s mode = '%s' (read 0x%02X)", this->get_name().c_str(), p.name.c_str(), byte);
        this->publish_state();
        return;
      }
    }
  }
  // A state byte with no writable counterpart (the read enum can be a superset
  // of the write enum) lands here: keep the last preset, surface it in the log.
  ESP_LOGW(TAG, "%s: device mode 0x%02X is not in any preset's read set", this->get_name().c_str(), byte);
}

void VitoClimate::dump_config() {
  ESP_LOGCONFIG(TAG, "VitoHome Climate '%s'", this->get_name().c_str());
  ESP_LOGCONFIG(TAG, "  Setpoint address: 0x%04X  range: %d..%d degC", this->setpoint_.get_datapoint().address(),
                this->setpoint_min_, this->setpoint_max_);
  if (this->has_mode_) {
    ESP_LOGCONFIG(TAG, "  Mode read 0x%04X  write 0x%04X  presets: %zu", this->mode_.get_datapoint().address(),
                  this->mode_.get_write_datapoint().address(), this->presets_.size());
  } else {
    ESP_LOGCONFIG(TAG, "  Mode: setpoint-only (no operating_mode block)");
  }
}

}  // namespace vitohome
}  // namespace esphome
