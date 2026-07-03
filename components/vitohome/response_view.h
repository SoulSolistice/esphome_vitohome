#pragma once

#include <cstdint>

namespace esphome::vitohome {

// Protocol-agnostic view of a decoded response payload. The ProtocolAdapter
// builds one of these from whichever concrete packet / callback shape the
// selected engine produces, so the hub and entities never depend on a
// protocol-specific packet type.
struct ResponseView {
  const uint8_t* data;
  uint8_t data_length;
  // P300: the address echoed in the response frame itself (a real wire-level
  // datum the hub matches against the in-flight request). KW/GWG: those
  // protocols carry no address in the response, so this is the request's
  // address -- the hub's address match degenerates to a tautology there.
  uint16_t address;
};

}  // namespace esphome::vitohome
