#pragma once

// A LinkSink that only counts (docs/design_notes.md, section 9b). The call
// layers (kw_calls.h, gwg_calls.h) attach it to every live exchange they
// wire, so the differential target holds the engines to the recorded traces
// with the trace on, and every proof on those layers runs with it.

#include <cstdint>

#include "optolink_types.h"

namespace trace_sink {

inline uint32_t &count() {
  static uint32_t n = 0;
  return n;
}

inline esphome::vitohome::optolink::LinkSink counting() {
  return esphome::vitohome::optolink::LinkSink{
      +[](void *, esphome::vitohome::optolink::LinkTrace, uint8_t, uint16_t) { ++count(); }, nullptr};
}

}  // namespace trace_sink
