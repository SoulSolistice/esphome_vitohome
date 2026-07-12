#pragma once

#include <cstdint>

namespace esphome::vitohome {

// Protocol-agnostic view of a decoded response payload. The hub builds one of
// these in its engine onResponse registration (all three engines deliver the
// same (data, length, address) callback), so the entities never depend on the
// engine callback shape.
struct ResponseView {
  const uint8_t *data;
  uint8_t data_length;
  // P300: the address echoed in the response frame itself (a real wire-level
  // datum the hub matches against the in-flight request). KW/GWG: those
  // protocols carry no address in the response, so the engine echoes the
  // request's address -- the hub's address match degenerates to a tautology
  // there.
  uint16_t address;
};

}  // namespace esphome::vitohome
