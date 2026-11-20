#pragma once

// A frame log as data: the >>> (engine) and <<< (device) lines of an ESPHome
// log, with their bytes and times, as import_frames.py writes them.

#include <cstddef>
#include <cstdint>

namespace sim {

struct TraceLine {
  uint32_t t_ms;    // from the first line
  bool device;      // <<<: the device sent it; >>>: the engine
  uint32_t offset;  // into Trace::bytes
  uint8_t length;
};

struct Trace {
  const char *name;
  const char *source;
  const TraceLine *lines;
  std::size_t count;
  const uint8_t *bytes;
};

}  // namespace sim
