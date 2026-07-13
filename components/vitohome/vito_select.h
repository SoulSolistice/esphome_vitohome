#pragma once
#include "esphome/core/defines.h"

#ifdef USE_SELECT
#include <vector>

#include "esphome/components/select/select.h"
#include "esphome/core/component.h"
#include "vito_entity.h"

namespace esphome::vitohome {

// Writable enum datapoint exposed as an ESPHome select. The option labels
// live in SelectTraits (set by codegen); raw_values_ is the parallel list of
// wire values in the same order. Only the index-based control() is
// overridden — that is the non-deprecated path in current ESPHome (the
// string-based default forwards to it).
class VitoSelect final : public select::Select, public Component, public VitoEntityBase {
 public:
  void add_raw_value(uint32_t value) { this->raw_values_.push_back(value); }
  void set_read_back(bool v) { this->read_back_ = v; }
  // Aligned block extraction on the state read (mirrors VitoSensor):
  // `set_extract_byte` marks the field's start inside the block read,
  // `set_extract_len` the field width (1..2 bytes, default 1). The write
  // datapoint carries the field width (set by codegen), so control() is
  // untouched by extraction.
  void set_extract_byte(int16_t byte) { this->extract_byte_ = byte; }
  void set_extract_len(uint8_t len) { this->extract_len_ = len; }

  void dump_config() override;
  void handle_response(const ResponseView &response) override;
  void handle_write_response(const ResponseView &response) override;
  void handle_error(optolink::OptolinkResult error) override;
  const char *entity_kind() const override { return "select"; }

 protected:
  void control(size_t index) override;

  std::vector<uint32_t> raw_values_;
  size_t pending_index_{0};
  int16_t extract_byte_{-1};
  uint8_t extract_len_{1};  // field width to slice at extract_byte_ (1..2)
};

}  // namespace esphome::vitohome
#endif  // USE_SELECT
