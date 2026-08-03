#pragma once
#include "esphome/core/defines.h"

#ifdef USE_EVENT
#include "esphome/components/event/event.h"
#include "esphome/core/component.h"
#include "vito_entity.h"

namespace esphome::vitohome {

// Fault-event entity: polls a fault-history slot (typically slot 1 of the
// Vitotronic SYSTEM archive -- the newest fault, 0x7507 on the B3HA: code byte
// + 8-byte BCD timestamp; NOT the FehlerHisFA* GFA burner archive, whose codes
// live in a different space -- see design_notes.md SS7) and fires
// a Home Assistant event when the code CHANGES -- a new fault fires its hex
// code ("0x10"), a cleared slot fires "cleared", a code outside the
// configured set fires "unknown" with the raw value in the log. This lands in
// HA's logbook natively and complements the polling error_history
// text_sensor, which shows the current slot contents but cannot notify.
//
// The FIRST successful poll only records a baseline and never fires:
// whatever fault happens to sit in the slot at boot would otherwise spam the
// logbook on every reboot.
class VitoEvent final : public event::Event, public Component, public VitoEntityBase {
 public:
  // Fault-code table as a codegen-emitted static array in .rodata (VitoOption):
  // pointer + count, no heap. Code 0x00 is filtered out by event.py -- it fires
  // the built-in "cleared" type and never reaches label_for_().
  void set_codes(const VitoOption *codes, uint16_t count) {
    this->codes_ = codes;
    this->code_count_ = count;
  }

  void dump_config() override;
  void handle_response(const ResponseView &response) override;
  void handle_error(optolink::OptolinkResult error) override;
  const char *entity_kind() const override { return "event"; }

 protected:
  const char *label_for_(uint8_t code) const;

  const VitoOption *codes_{nullptr};
  uint16_t code_count_{0};
  uint8_t last_code_{0};
  bool baseline_set_{false};
};

}  // namespace esphome::vitohome
#endif  // USE_EVENT
