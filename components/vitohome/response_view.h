#pragma once

#include <cstdint>

namespace esphome {
namespace vitohome {

// Protocol-agnostic view of a decoded response payload. The ProtocolAdapter
// builds one of these from whichever concrete packet / callback shape the
// selected engine produces, so the hub and entities never depend on a
// protocol-specific packet type.
struct ResponseView {
  const uint8_t *data;
  uint8_t data_length;
  uint16_t address;
};

}  // namespace vitohome
}  // namespace esphome
