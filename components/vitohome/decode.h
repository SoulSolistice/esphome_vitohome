#pragma once
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace vitohome {

// Pure, framework-free decode helpers, kept separate so the range/bit
// logic can be unit-tested on the host without VitoWiFi or ESPHome
// headers. Returns false (and leaves *out untouched) if byte_offset is
// out of range for data_len.
inline bool decode_masked_bit(const uint8_t *data, std::size_t data_len, uint8_t byte_offset, uint8_t bit_mask,
                              bool *out) {
  if (data == nullptr || data_len <= byte_offset) return false;
  *out = (data[byte_offset] & bit_mask) != 0;
  return true;
}

}  // namespace vitohome
}  // namespace esphome
