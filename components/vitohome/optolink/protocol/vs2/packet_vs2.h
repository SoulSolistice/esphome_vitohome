/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.

Buffer modernization: the upstream malloc/free/realloc + _allocatedLength
is replaced by a fixed std::array<uint8_t, kMaxFrame>. The VS2 length byte
is a uint8_t and the internal buffer stores the length byte plus the bytes
it counts (the lead-in 0x41 and the transmitted checksum are NOT stored),
so 1 + 255 = 256 is the exact, protocol-complete bound. With the raw
buffer gone the class is trivially copyable again, so the deleted copy
operations are restored.
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

namespace internals {
class ParserVS2;
}  // namespace internals

class PacketVS2 {
  friend class internals::ParserVS2;

 public:
  static constexpr std::size_t kMaxFrame = 256;

  PacketVS2();
  ~PacketVS2() = default;
  PacketVS2(const PacketVS2&) = default;
  PacketVS2& operator=(const PacketVS2&) = default;
  operator bool() const;
  uint8_t& operator[](std::size_t index);

 public:
  bool createPacket(PacketType pt, FunctionCode fc, uint8_t id, uint16_t addr, uint8_t len,
                    const uint8_t* data = nullptr);
  bool setLength(uint8_t length);
  uint8_t length() const;
  PacketType packetType() const;
  FunctionCode functionCode() const;
  uint8_t id() const;
  uint16_t address() const;
  uint8_t dataLength() const;
  const uint8_t* data() const;

  uint8_t checksum() const;

  void reset();

 protected:
  std::array<uint8_t, kMaxFrame> _buffer;
};

}  // namespace optolink
}  // namespace vitohome
}  // namespace esphome
