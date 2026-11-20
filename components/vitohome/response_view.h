#pragma once

#include <cstdint>

namespace esphome::vitohome {

// Protocol-agnostic view of a decoded response payload, built from the
// engine's Result, so the entities never depend on the engine.
struct ResponseView {
  const uint8_t *data;
  uint8_t data_length;
  // P300: the address echoed in the response frame itself (a real wire-level
  // datum the hub matches against the in-flight request). KW/GWG: those
  // protocols carry no address in the response, so the engine echoes the
  // request's address -- the hub's address match degenerates to a tautology
  // there.
  uint16_t address;
  // GWG: an EEPROM cell answered FF FF (not in use), delivered as FF.
  bool unserved_eeprom;
};

}  // namespace esphome::vitohome
