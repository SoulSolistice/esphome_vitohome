/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#include "packet_vs1.h"

namespace esphome {
namespace vitohome {
namespace optolink {

PacketVS1::PacketVS1() : _buffer{} { reset(); }

PacketVS1::operator bool() const {
  if (_buffer[3] != 0) return true;
  return false;
}

uint8_t &PacketVS1::operator[](std::size_t index) { return _buffer[index]; }

bool PacketVS1::createPacket(uint8_t packetType, uint16_t addr, uint8_t len, const uint8_t *data) {
  reset();

  // check arguments
  if (len == 0) {
    return false;
  }
  if (packetType != PacketVS1Type.READ && packetType != PacketVS1Type.WRITE) {
    return false;
  }
  if (packetType == PacketVS1Type.WRITE && !data) {
    return false;
  }

  // bounds check against the fixed buffer (fail-soft, no overflow)
  const std::size_t needed = (packetType == PacketVS1Type.WRITE) ? static_cast<std::size_t>(len) + 4 : 4;
  if (needed > _buffer.size()) {
    optolink_log_e("buffer overflow: need %u > %u", static_cast<unsigned>(needed),
                   static_cast<unsigned>(_buffer.size()));
    return false;
  }

  // Serialize into buffer
  size_t step = 0;
  _buffer[step++] = packetType;
  _buffer[step++] = (addr >> 8) & 0xFF;
  _buffer[step++] = addr & 0xFF;
  _buffer[step++] = len;
  if (packetType == PacketVS1Type.WRITE) {
    for (uint8_t i = 0; i < len; ++i) {
      _buffer[step++] = data[i];
    }
  }
  return true;
}

uint8_t PacketVS1::length() const {
  if (_buffer[3] == 0) return 0;
  if (_buffer[0] == PacketVS1Type.READ) return 4;
  if (_buffer[0] == PacketVS1Type.WRITE) return _buffer[3] + 4;
  return 0;  // should not be possible
}

uint8_t PacketVS1::packetType() const { return _buffer[0]; }

uint16_t PacketVS1::address() const {
  uint16_t retVal = _buffer[1] << 8;
  retVal |= _buffer[2];
  return retVal;
}

uint8_t PacketVS1::dataLength() const { return _buffer[3]; }

const uint8_t *PacketVS1::data() const { return &_buffer[4]; }

void PacketVS1::reset() { _buffer[3] = 0x00; }

}  // namespace optolink
}  // namespace vitohome
}  // namespace esphome
