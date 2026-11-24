#pragma once

#include <cstdint>

namespace esphome::vitohome {

// An address and a length on the device, and a name for the log. The entity
// decodes and encodes the bytes itself (decode.h).
class Datapoint {
 public:
  constexpr Datapoint(const char *name, uint16_t address, uint8_t length)
      : name_(name), address_(address), length_(length) {}

  const char *name() const { return this->name_; }
  uint16_t address() const { return this->address_; }
  uint8_t length() const { return this->length_; }

 private:
  const char *name_;
  uint16_t address_;
  uint8_t length_;
};

}  // namespace esphome::vitohome
