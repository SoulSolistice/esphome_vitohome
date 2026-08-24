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
  // Access-mode divergence from upstream (see GWGAccessMode in constants.h):
  // upstream (and vitohome before this change) only ever emitted
  // PacketGWGType.READ (PHYSICAL READ, 0xCB). isKnownGwgReadType() accepts
  // that plus the other seven read-direction TYPE bytes from the openv wiki
  // table; every WRITE-direction byte except PacketGWGType.WRITE itself is
  // still rejected, exactly as before this change.
  if (!isKnownGwgReadType(packetType) && packetType != PacketGWGType.WRITE) {
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
  // Bug fixed 2026-08-24 (caught by proof_gwg_access_mode.cpp): this compared
  // only against PacketGWGType.READ (0xCB, PHYSICAL READ), so createPacket()
  // would accept any other read-access-mode TYPE byte (isKnownGwgReadType())
  // but length() then fell through to `return 0` for it -- _send() then wrote
  // zero bytes and the engine transitioned straight to RECEIVE with nothing on
  // the wire. Every read TYPE byte is the same 5-byte frame shape
  // (01 TYPE ADDR LEN 04); only WRITE carries a variable-length payload.
  if (isKnownGwgReadType(_buffer[1]))
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
