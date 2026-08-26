#pragma once
#include "esphome/core/defines.h"

#ifdef USE_CLIMATE
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
class VitoClimateChannel final : public VitoEntityBase {
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

// One Betriebsart preset, emitted as a codegen row in a `static const
// VitoClimatePreset[]` table (.rodata). The binding between the command and
// state spaces is positional: this row says "write_value (command space) and
// any of read_values (state space) are the same operating mode, displayed as
// `mode`." `name` is a free label. Deliberately a POD of pointers: the previous
// shape held a std::string AND a std::vector<uint8_t> per preset, so every
// preset cost two heap blocks (plus the presets_ vector's own growth) -- ~10
// allocations for the 5-preset curated example. `name` and `read_values` point
// at codegen literals with static storage; nothing here is owned.
struct VitoClimatePreset {
  const char *name;
  uint8_t write_value;
  const uint8_t *read_values;
  uint8_t read_count;
  climate::ClimateMode mode;
};

// Weather-compensated heating-circuit proxy. The slider writes the room
// setpoint (the boiler applies it through its heat curve), and Betriebsart is
// exposed as custom presets with a coarse climate mode derived from the active
// preset. Every surface (this card, a select, the boiler panel) is a view over
// the device registers; writes propagate and the read-back reconciles all of
// them, so there is no ownership conflict -- only the preset table guarantees
// each state is representable.
class VitoClimate final : public climate::Climate, public Component {
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

  // GWG access mode, per channel (2026-08-26). The two channels address
  // different registers -- the room setpoint and Betriebsart -- and nothing
  // says they live in the same GWG access space, so they are set separately
  // rather than one climate-wide mode. As everywhere else, one mode drives
  // both directions of a channel (see VitoEntityBase::access_); the default is
  // PHYSICAL, i.e. byte-identical to the behaviour before this existed.
  void set_setpoint_access(optolink::GWGAccessMode access) { this->setpoint_.set_access(access); }
  void set_mode_access(optolink::GWGAccessMode access) { this->mode_.set_access(access); }
  // The whole preset table at once, as a static array in .rodata.
  void set_presets(const VitoClimatePreset *presets, uint16_t count) {
    this->presets_ = presets;
    this->preset_count_ = count;
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
  const VitoClimatePreset *presets_{nullptr};
  uint16_t preset_count_{0};
  bool has_mode_{false};
  int setpoint_min_{3};
  int setpoint_max_{37};
};

}  // namespace esphome::vitohome
#endif  // USE_CLIMATE
