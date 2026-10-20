#pragma once
#include "esphome/core/defines.h"

#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#include "esphome/core/component.h"
#include "vito_entity.h"

namespace esphome::vitohome {

class VitoHomeComponent;

// Force-refresh button: pressing it calls the hub's refresh_all(), which
// marks every registered datapoint due on the next scheduler tick (reads
// only; the existing queue discipline throttles the burst). Usable from the
// HA UI and from automations via button.press; ESPHome-side automations can
// call id(vito).refresh_all() directly instead.
class VitoRefreshButton final : public button::Button, public Component {
 public:
  void set_vitohome_parent(VitoHomeComponent *parent) { this->vh_parent_ = parent; }
  void dump_config() override;

 protected:
  void press_action() override;

  VitoHomeComponent *vh_parent_{nullptr};
};

// Action button (`type: action`, `access: rpc`, P300 only): each press is one
// remote procedure call -- function code 0x07 with the catalog's parameter
// (`prefix:`, empty for a procedure without one) -- that RUNS a procedure on
// the controller: clear the fault history, reset a circuit's parameters, step
// a boiler sequence. It rides the hub's write lane like any setting, so it
// waits for the bus and is never interleaved with a read-modify-write. It is
// never polled and never read back (reading a procedure would run it again);
// once the controller acknowledges, the hub re-reads every value, so what the
// procedure changed shows up.
class VitoActionButton final : public button::Button, public Component, public VitoEntityBase {
 public:
  VitoActionButton() { this->read_back_ = false; }

  bool wants_polling() const override { return false; }
  void handle_response(const ResponseView & /*response*/) override {}
  void handle_error(optolink::OptolinkResult /*error*/) override {}
  void handle_write_response(const ResponseView &response) override;
  void handle_write_error(optolink::OptolinkResult error) override;
  const char *entity_kind() const override { return "action"; }
  void dump_config() override;

 protected:
  void press_action() override;
};

}  // namespace esphome::vitohome
#endif  // USE_BUTTON
