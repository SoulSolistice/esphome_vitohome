/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.

Buffer modernization: the upstream malloc/free/realloc + _allocatedLength
is replaced by a fixed std::array<uint8_t, kMaxFrame>. A VS1 write stores
its payload after a 4-byte header (type, addr-hi, addr-lo, len), and the
length byte is a uint8_t, so 4 + 255 = 259 bounds the buffer; 256 is the
datapoint length cap used elsewhere, so kMaxFrame = 260 covers every valid
frame with margin. With the raw buffer gone the deleted copy operations
are restored.
*/

#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "../../constants.h"
#include "../../helpers.h"
#include "../../logging.h"

namespace esphome {
namespace vitohome {
namespace optolink {

class PacketVS1 {
 public:
  static constexpr std::size_t kMaxFrame = 260;

  PacketVS1();
  ~PacketVS1() = default;
  PacketVS1(const PacketVS1 &) = default;
  PacketVS1 &operator=(const PacketVS1 &) = default;
  operator bool() const;
  uint8_t &operator[](std::size_t index);

 public:
  bool createPacket(uint8_t packetType, uint16_t addr, uint8_t len, const uint8_t *data = nullptr);
  uint8_t length() const;
  uint8_t packetType() const;
  uint16_t address() const;
  uint8_t dataLength() const;
  const uint8_t *data() const;

  void reset();

 protected:
  std::array<uint8_t, kMaxFrame> _buffer;
};

}  // namespace optolink
}  // namespace vitohome
}  // namespace esphome
