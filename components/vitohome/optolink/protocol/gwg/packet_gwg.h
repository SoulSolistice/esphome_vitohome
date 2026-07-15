/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.

Buffer modernization: malloc/free/realloc + _allocatedLength replaced by a
fixed std::array<uint8_t, kMaxFrame>. A GWG write stores its payload after a
4-byte header (ENQ_ACK, type, addr, len) and is terminated by an EOT byte,
with a uint8_t length, so kMaxFrame = 260 covers every valid frame with
margin. Deleted copy operations restored.
*/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../constants.h"
#include "../../logging.h"

namespace esphome::vitohome::optolink {

class PacketGWG {
 public:
  static constexpr std::size_t kMaxFrame = 260;

  PacketGWG();
  ~PacketGWG() = default;
  PacketGWG(const PacketGWG &) = default;
  PacketGWG &operator=(const PacketGWG &) = default;
  uint8_t &operator[](std::size_t index);

 public:
  bool createPacket(uint8_t packetType, uint16_t addr, uint8_t len, const uint8_t *data = nullptr);
  uint8_t length() const;
  uint8_t packetType() const;

  void reset();

 protected:
  std::array<uint8_t, kMaxFrame> _buffer;
};

}  // namespace esphome::vitohome::optolink
