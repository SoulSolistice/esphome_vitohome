/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#include "packet_gwg.h"

namespace esphome::vitohome::optolink {

PacketGWG::PacketGWG() : _buffer{} { reset(); }

uint8_t &PacketGWG::operator[](std::size_t index) { return _buffer[index]; }

bool PacketGWG::createPacket(uint8_t packetType, uint16_t addr, uint8_t len, const uint8_t *data) {
  reset();

  // check arguments
  if (len == 0) {
    optolink_log_w("Zero length given");
    return false;
  }
  if (addr > 0xFF) {
    optolink_log_w("GWG doesn't support addresses > 0xFF");
    return false;
  }
  if (packetType != PacketGWGType.READ && packetType != PacketGWGType.WRITE) {
    optolink_log_w("Packet type error: 0x%02x", packetType);
    return false;
  }
  if (packetType == PacketGWGType.WRITE && !data) {
    optolink_log_w("No data for write packet");
    return false;
  }

  // bounds check against the fixed buffer (fail-soft, no overflow)
  // write frame = ENQ_ACK + type + addr + len + data[len] + EOT
  const std::size_t needed = (packetType == PacketGWGType.WRITE) ? static_cast<std::size_t>(len) + 5 : 5;
  if (needed > _buffer.size()) {
    optolink_log_e("buffer overflow: need %u > %u", static_cast<unsigned>(needed),
                   static_cast<unsigned>(_buffer.size()));
    return false;
  }

  // Serialize into buffer
  size_t step = 0;
  _buffer[step++] = internals::ProtocolBytes.ENQ_ACK;
  _buffer[step++] = packetType;
  _buffer[step++] = addr & 0xFF;
  _buffer[step++] = len;
  if (packetType == PacketGWGType.WRITE) {
    for (uint8_t i = 0; i < len; ++i) {
      _buffer[step++] = data[i];
    }
  }
  _buffer[step] = internals::ProtocolBytes.EOT;
  return true;
}

uint8_t PacketGWG::length() const {
  if (_buffer[3] == 0)
    return 0;
  if (_buffer[1] == PacketGWGType.READ)
    return 5;
  // NOTE: uint8_t arithmetic -- wraps for a payload length >= 251.
  // Unreachable today (the raw lane caps writes at 32 bytes; entity writes are
  // <= 8), but a live trap if those caps are ever raised. Mirrors the same note
  // on PacketVS1::length() (whose +4 wraps at >= 252).
  if (_buffer[1] == PacketGWGType.WRITE)
    return _buffer[3] + 5;
  return 0;  // should not be possible
}

uint8_t PacketGWG::packetType() const { return _buffer[1]; }

void PacketGWG::reset() { _buffer[3] = 0x00; }

}  // namespace esphome::vitohome::optolink
