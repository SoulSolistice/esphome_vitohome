#pragma once
#include <string>
#include <vector>

#include "esphome/components/climate/climate.h"
#include "esphome/core/component.h"
#include "vito_entity.h"

namespace esphome::vitohome {

class VitoClimate;

// Internal, hub-polled wire channel owned by a VitoClimate. It is NOT a Home
// Assistant entity -- it exists only so the hub's proven read / write /
// read-back machinery services the climate's registers. A climate owns up to
// two channels: SETPOINT (the room setpoint, read address == write address)
// and MODE (Betriebsart, whose live state is read at one address and commanded
// at another -- the read/write split). Reads are handed to the parent climate;
// writes are staged as a single raw byte and queued through the hub.
class VitoClimateChannel : public VitoEntityBase {
 public:
  enum Kind : uint8_t { SETPOINT, MODE };
  VitoClimateChannel(VitoClimate *parent, Kind kind) : parent_(parent), kind_(kind) {}

  void set_read_back(bool v) { this->read_back_ = v; }

  void handle_response(const ResponseView &response) override;
  void handle_error(optolink::OptolinkResult /*error*/) override {}          // keep last state
  void handle_write_response(const ResponseView & /*response*/) override {}  // read-back reconciles
  const char *entity_kind() const override { return "climate"; }
  void dump_config() override {}

  // Stage one raw byte and queue the write through the hub (to the write
  // datapoint when set, else the read/state datapoint).
  bool write_byte(uint8_t value);

 protected:
  VitoClimate *parent_;
  Kind kind_;
};

// One Betriebsart preset. The binding between the spaces is positional: this
// row says "write_value (command space) and any of read_values (state space)
// are the same operating mode, displayed as `mode`." `name` is a free label.
struct VitoClimatePreset {
  std::string name;
  uint8_t write_value;
  std::vector<uint8_t> read_values;
  climate::ClimateMode mode;
};

// Weather-compensated heating-circuit proxy. The slider writes the room
// setpoint (the boiler applies it through its heat curve), and Betriebsart is
// exposed as custom presets with a coarse climate mode derived from the active
// preset. Every surface (this card, a select, the boiler panel) is a view over
// the device registers; writes propagate and the read-back reconciles all of
// them, so there is no ownership conflict -- only the preset table guarantees
// each state is representable.
class VitoClimate : public climate::Climate, public Component {
 public:
  VitoClimate() : setpoint_(this, VitoClimateChannel::SETPOINT), mode_(this, VitoClimateChannel::MODE) {}

  // --- codegen wiring -------------------------------------------------------
  // The climate owns its channels; codegen passes only primitives/datapoints,
  // never raw channel pointers (chaining on a pointer-returning accessor would
  // generate invalid C++). configure_* set up the channel and register it with
  // the hub.
  void set_setpoint_range(int min_c, int max_c) {
    this->setpoint_min_ = min_c;
    this->setpoint_max_ = max_c;
  }
  void configure_setpoint(VitoHomeComponent *hub, const optolink::Datapoint &dp, uint32_t poll_ms);
  void configure_mode(VitoHomeComponent *hub, const optolink::Datapoint &read_dp, bool read_back, uint32_t poll_ms);
  void set_mode_write_datapoint(const optolink::Datapoint &dp) { this->mode_.set_write_datapoint(dp); }
  void add_preset(const std::string &name, uint8_t write_value, const std::vector<uint8_t> &read_values,
                  climate::ClimateMode mode) {
    this->presets_.push_back(VitoClimatePreset{name, write_value, read_values, mode});
  }

  // --- Component / Climate --------------------------------------------------
  void setup() override;
  void dump_config() override;
  void control(const climate::ClimateCall &call) override;
  climate::ClimateTraits traits() override;

  // --- channel read callbacks ----------------------------------------------
  void on_setpoint_read(const ResponseView &response);
  void on_mode_read(const ResponseView &response);

 protected:
  const VitoClimatePreset *find_preset_by_name_(const char *name) const;
  const VitoClimatePreset *first_preset_with_mode_(climate::ClimateMode mode) const;

  VitoClimateChannel setpoint_;
  VitoClimateChannel mode_;
  std::vector<VitoClimatePreset> presets_;
  bool has_mode_{false};
  int setpoint_min_{3};
  int setpoint_max_{37};
};

}  // namespace esphome::vitohome
