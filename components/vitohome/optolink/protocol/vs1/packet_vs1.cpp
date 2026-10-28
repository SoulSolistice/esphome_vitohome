/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#include "packet_vs1.h"

namespace esphome::vitohome::optolink {

PacketVS1::PacketVS1() : _buffer{} { reset(); }

uint8_t &PacketVS1::operator[](std::size_t index) { return _buffer[index]; }

bool PacketVS1::createPacket(uint8_t packetType, uint16_t addr, uint8_t len, const uint8_t *data) {
  reset();

  // The three telegrams the engine's own requests use; anything else goes
  // through createTelegram().
  if (packetType != PacketVS1Type.READ && packetType != PacketVS1Type.WRITE && packetType != PacketVS1Type.GFA_READ) {
    return false;
  }
  if (packetType == PacketVS1Type.WRITE && !data) {
    return false;
  }
  return createTelegram(packetType, addr, len, packetType == PacketVS1Type.WRITE ? data : nullptr);
}

// Any type byte: `type addr-hi addr-lo len`, followed by `len` payload bytes
// when `data` is given (the shape of a write). The frame length is fixed here
// rather than derived from the type byte later, so a type the engine has no
// name for still goes out whole (the raw lane, VS1Engine::telegram()).
bool PacketVS1::createTelegram(uint8_t type, uint16_t addr, uint8_t len, const uint8_t *data) {
  reset();

  // check arguments
  if (len == 0) {
    return false;
  }

  // bounds check against the fixed buffer (fail-soft, no overflow)
  const std::size_t needed = data ? static_cast<std::size_t>(len) + 4 : 4;
  if (needed > _buffer.size()) {
    optolink_log_e("buffer overflow: need %u > %u", static_cast<unsigned>(needed),
                   static_cast<unsigned>(_buffer.size()));
    return false;
  }

  // Serialize into buffer
  size_t step = 0;
  _buffer[step++] = type;
  _buffer[step++] = (addr >> 8) & 0xFF;
  _buffer[step++] = addr & 0xFF;
  _buffer[step++] = len;
  if (data) {
    for (uint8_t i = 0; i < len; ++i) {
      _buffer[step++] = data[i];
    }
  }
  _length = static_cast<uint8_t>(step);
  _hasPayload = data != nullptr;
  return true;
}

// NOTE: uint8_t -- a payload of 252+ bytes would wrap. Unreachable: the raw
// lane caps payloads at 32 and entity writes at 8.
uint8_t PacketVS1::length() const { return _length; }

bool PacketVS1::hasPayload() const { return _hasPayload; }

uint8_t PacketVS1::packetType() const { return _buffer[0]; }

void PacketVS1::reset() {
  _buffer[3] = 0x00;
  _length = 0;
  _hasPayload = false;
}

}  // namespace esphome::vitohome::optolink
