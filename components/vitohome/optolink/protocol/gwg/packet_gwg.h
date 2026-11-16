/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.

Buffer modernization: malloc/free/realloc + _allocatedLength replaced by a
fixed std::array<uint8_t, kMaxFrame>: a 4-byte header (ENQ_ACK, type,
addr, len), at most MAX_PAYLOAD payload bytes and an EOT. Deleted copy
operations restored.

Access modes (2026-08-24, see GWGAccessMode in constants.h): createPacket()
accepts any of the eight read-direction TYPE bytes rather than only the
PHYSICAL READ 0xCB it took before, and length() reports the same 5-byte frame
shape for all of them. createTelegram() (2026-10-27) builds a frame for any
TYPE byte, for the raw lane; createPacket() still rejects the bytes outside
the access-mode table.
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
  static constexpr std::size_t kMaxFrame = 5 + MAX_PAYLOAD;
  // length() and the engine's byte count are uint8_t.
  static_assert(kMaxFrame <= 0xFF, "a frame must fit a uint8_t length");

  PacketGWG();
  ~PacketGWG() = default;
  PacketGWG(const PacketGWG &) = default;
  PacketGWG &operator=(const PacketGWG &) = default;
  uint8_t &operator[](std::size_t index);

 public:
  bool createPacket(uint8_t packetType, uint16_t addr, uint8_t len, const uint8_t *data = nullptr);
  bool createTelegram(uint8_t type, uint16_t addr, uint8_t len, const uint8_t *data = nullptr);
  // An EEPROM write of one cell, `01 AD ADDR b0 b1 04`, the two bytes stored
  // as the cell's pair as given.
  bool createEepromPair(uint16_t addr, uint8_t b0, uint8_t b1);
  // `len` bytes exactly as given, nothing added: an open frame for the raw
  // lane. Its answer is data, not an acknowledgement.
  bool createFrame(const uint8_t *bytes, uint8_t len);
  uint8_t length() const;
  uint8_t packetType() const;
  // Whether the frame carries `len` payload bytes (a write's shape): its
  // answer is then a one-byte acknowledgement, otherwise `len` data bytes.
  bool hasPayload() const;
  // Write frame layout for the packets built from now on: EOT after the
  // payload (false) or before it (true). See GWGEngine::setWriteEotBeforePayload().
  void setEotBeforePayload(bool eotFirst) { _eotBeforePayload = eotFirst; }
  bool eotBeforePayload() const { return _eotBeforePayload; }

  void reset();

 protected:
  std::array<uint8_t, kMaxFrame> _buffer;
  uint8_t _length{0};
  bool _hasPayload{false};
  bool _eotBeforePayload{false};
};

}  // namespace esphome::vitohome::optolink
