#include "vito_climate.h"
#ifdef USE_CLIMATE

#include <cmath>
#include <cstring>
#include <vector>

#include "decode.h"
#include "esphome/core/log.h"
#include "vitohome.h"

namespace esphome::vitohome {

static const char *const TAG = "vitohome.climate";

// --- channel ---------------------------------------------------------------

bool VitoClimateChannel::write_byte(uint8_t value) {
  if (!this->set_write_payload_(&value, 1))
    return false;
  return this->vh_parent_ != nullptr && this->vh_parent_->request_write(this);
}

void VitoClimateChannel::handle_response(const ResponseView &response) {
  if (this->kind_ == SETPOINT) {
    this->parent_->on_setpoint_read(response);
  } else {
    this->parent_->on_mode_read(response);
  }
}

void VitoClimateChannel::handle_write_response(const ResponseView & /*response*/) {
  // With read_back on, the hub's read-back publishes the device's view.
  if (this->wants_read_back() || this->write_length() < 1)
    return;
  if (this->kind_ == SETPOINT) {
    this->parent_->on_setpoint_written(this->write_data()[0]);
  } else {
    this->parent_->on_mode_written(this->write_data()[0]);
  }
}

void VitoClimateChannel::handle_write_error(optolink::OptolinkResult error) {
  // Nothing was published for the request, so there is nothing to undo.
  ESP_LOGW(TAG, "%s: %s write failed (%s); state unchanged", this->parent_->get_name().c_str(),
           this->kind_ == SETPOINT ? "setpoint" : "mode", optolink::errorToString(error));
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
  if (this->has_mode_ && this->preset_count_ > 0) {
    std::vector<const char *> names;
    names.reserve(this->preset_count_);
    for (uint16_t i = 0; i < this->preset_count_; i++)
      names.push_back(this->presets_[i].name);
    this->set_supported_custom_presets(names);
  }
  // The coarse mode until a read maps it, and what stays published when the
  // first mode read matches no preset. A heating circuit in an unmapped
  // Betriebsart (holiday program, external switching) is mostly still heating,
  // so HEAT where the presets offer it; OFF only where no preset does.
  this->mode = this->offers_heat_() ? climate::CLIMATE_MODE_HEAT : climate::CLIMATE_MODE_OFF;
}

bool VitoClimate::offers_heat_() const {
  return !this->has_mode_ || this->first_preset_with_mode_(climate::CLIMATE_MODE_HEAT) != nullptr;
}

climate::ClimateTraits VitoClimate::traits() {
  climate::ClimateTraits t;
  // No current temperature: the boiler controls weather-compensated, so the
  // card is target-only. The modes are the ones a preset can write (OFF is in
  // the mask by default); a setpoint-only climate is HEAT.
  if (this->offers_heat_())
    t.add_supported_mode(climate::CLIMATE_MODE_HEAT);
  if (this->has_mode_) {
    for (uint16_t i = 0; i < this->preset_count_; i++)
      t.add_supported_mode(this->presets_[i].mode);
  }
  // The gauge spans the range control() clamps to, on the 1 degC grid of the
  // 1-byte setpoint register. A user `visual:` block overrides it.
  t.set_visual_min_temperature(static_cast<float>(this->setpoint_min_));
  t.set_visual_max_temperature(static_cast<float>(this->setpoint_max_));
  t.set_visual_target_temperature_step(1.0f);
  return t;
}

const VitoClimatePreset *VitoClimate::find_preset_by_name_(const char *name) const {
  for (uint16_t i = 0; i < this->preset_count_; i++) {
    if (std::strcmp(this->presets_[i].name, name) == 0)
      return &this->presets_[i];
  }
  return nullptr;
}

const VitoClimatePreset *VitoClimate::first_preset_with_mode_(climate::ClimateMode mode) const {
  for (uint16_t i = 0; i < this->preset_count_; i++) {
    if (this->presets_[i].mode == mode)
      return &this->presets_[i];
  }
  return nullptr;
}

// Publishes nothing: the request is only queued. The device's read-back (or,
// with read_back off, its acknowledgement) publishes the new state, so Home
// Assistant never shows a mode or setpoint the device has not accepted
// (design_notes.md §5c).
void VitoClimate::control(const climate::ClimateCall &call) {
  // Target temperature -> room setpoint (1-byte integer degC), clamped to the
  // configured range.
  if (call.get_target_temperature().has_value()) {
    const float t = *call.get_target_temperature();
    if (std::isnan(t)) {
      ESP_LOGW(TAG, "%s: target temperature is not a number; nothing written", this->get_name().c_str());
    } else {
      const uint8_t byte = setpoint_byte(t, this->setpoint_min_, this->setpoint_max_);
      if (!this->setpoint_.write_byte(byte)) {
        ESP_LOGE(TAG, "%s: setpoint write could not be queued", this->get_name().c_str());
      }
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
      if (!this->mode_.write_byte(p->write_value)) {
        ESP_LOGE(TAG, "%s: mode write could not be queued", this->get_name().c_str());
      }
    } else if (call.has_custom_preset() || call.get_mode().has_value()) {
      // A mode or preset the table cannot express (an OFF tap without an
      // OFF preset): there is no byte to write, so say why nothing happens.
      ESP_LOGW(TAG, "%s: no preset matches the requested mode/preset; nothing written (add a preset row for it)",
               this->get_name().c_str());
    }
  }
}

void VitoClimate::on_setpoint_written(uint8_t value) {
  this->target_temperature = static_cast<float>(value);
  this->setpoint_known_ = true;
  if (this->ready_())
    this->publish_state();
}

void VitoClimate::on_mode_written(uint8_t write_value) {
  for (uint16_t i = 0; i < this->preset_count_; i++) {
    if (this->presets_[i].write_value == write_value) {
      this->set_custom_preset_(this->presets_[i].name);
      this->mode = this->presets_[i].mode;
      this->mode_known_ = true;
      if (this->ready_())
        this->publish_state();
      return;
    }
  }
}

void VitoClimate::on_setpoint_read(const ResponseView &response) {
  if (response.data == nullptr || response.data_length < 1) {
    ESP_LOGW(TAG, "%s: setpoint response too short", this->get_name().c_str());
    return;
  }
  this->target_temperature = static_cast<float>(response.data[0]);
  this->setpoint_known_ = true;
  ESP_LOGD(TAG, "%s setpoint = %u degC", this->get_name().c_str(), response.data[0]);
  if (this->ready_())
    this->publish_state();
}

void VitoClimate::on_mode_read(const ResponseView &response) {
  if (response.data == nullptr || response.data_length < 1) {
    ESP_LOGW(TAG, "%s: mode response too short", this->get_name().c_str());
    return;
  }
  const uint8_t byte = response.data[0];
  for (uint16_t i = 0; i < this->preset_count_; i++) {
    const VitoClimatePreset &p = this->presets_[i];
    for (uint8_t j = 0; j < p.read_count; j++) {
      if (p.read_values[j] == byte) {
        this->set_custom_preset_(p.name);
        this->mode = p.mode;
        this->mode_known_ = true;
        ESP_LOGD(TAG, "%s mode = '%s' (read 0x%02X)", this->get_name().c_str(), p.name, byte);
        if (this->ready_())
          this->publish_state();
        return;
      }
    }
  }
  // No preset reads this byte: a device state the write enum lacks, or a
  // Betriebsart set at the panel or by the schedule that the preset table does
  // not cover. The previous preset is no longer true, so it is cleared. The
  // coarse mode keeps its last value (the setup() placeholder on the first
  // read): an unmapped byte has no principled bucket.
  //
  // The first read counts as "mode known" even when unmapped, so a working
  // setpoint is not withheld. The publish runs only on a change, not on every
  // poll of a device that stays unmapped.
  const bool first = !this->mode_known_;
  this->mode_known_ = true;
  const bool changed = this->has_custom_preset() || first;
  if (changed) {
    this->clear_custom_preset_();
    if (this->ready_())
      this->publish_state();
    ESP_LOGW(TAG, "%s: device mode 0x%02X is not in any preset's read set; preset cleared%s", this->get_name().c_str(),
             byte, first ? ", climate mode unconfirmed" : "");
  } else {
    ESP_LOGD(TAG, "%s: device mode 0x%02X still unmapped", this->get_name().c_str(), byte);
  }
}

void VitoClimate::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Climate '%s'\n"
                "  Setpoint address: 0x%04X  range: %d..%d degC",
                this->get_name().c_str(), this->setpoint_.get_datapoint().address(), this->setpoint_min_,
                this->setpoint_max_);
  if (this->has_mode_) {
    ESP_LOGCONFIG(TAG, "  Mode read 0x%04X  write 0x%04X  presets: %u", this->mode_.get_datapoint().address(),
                  this->mode_.get_write_datapoint().address(), this->preset_count_);
  } else {
    ESP_LOGCONFIG(TAG, "  Mode: setpoint-only (no operating_mode block)");
  }
}

}  // namespace esphome::vitohome
#endif  // USE_CLIMATE
