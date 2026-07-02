#pragma once
#include <vector>

#include "esphome/components/select/select.h"
#include "esphome/core/component.h"
#include "vito_entity.h"

namespace esphome {
namespace vitohome {

// Writable enum datapoint exposed as an ESPHome select. The option labels
// live in SelectTraits (set by codegen); raw_values_ is the parallel list of
// wire values in the same order. Only the index-based control() is
// overridden — that is the non-deprecated path in current ESPHome (the
// string-based default forwards to it).
class VitoSelect : public select::Select, public Component, public VitoEntityBase {
 public:
  void add_raw_value(uint32_t value) { this->raw_values_.push_back(value); }
  void set_read_back(bool v) { this->read_back_ = v; }

  void dump_config() override;
  void handle_response(const ResponseView& response) override;
  void handle_write_response(const ResponseView& response) override;
  void handle_error(optolink::OptolinkResult error) override;
  const char* entity_kind() const override { return "select"; }

 protected:
  void control(size_t index) override;

  std::vector<uint32_t> raw_values_;
  size_t pending_index_{0};
};

}  // namespace vitohome
}  // namespace esphome
