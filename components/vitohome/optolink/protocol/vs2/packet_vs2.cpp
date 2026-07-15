/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#include "packet_vs2.h"

namespace esphome::vitohome::optolink {

PacketVS2::PacketVS2() : _buffer{} { reset(); }

uint8_t &PacketVS2::operator[](std::size_t index) { return _buffer[index]; }

bool PacketVS2::createPacket(PacketType pt, FunctionCode fc, uint8_t id, uint16_t addr, uint8_t len,
                             const uint8_t *data) {
  reset();

  // check arguments
  if (len == 0) {
    optolink_log_w("Length error: %u", len);
    return false;
  }
  // A frame carries an inline payload when it is a WRITE or a RESPONSE (a
  // read response echoes the data back). Every payload guard below keys on
  // this, matching the copy loop at the end. Keying the guards on WRITE alone
  // (the upstream shape) let a RESPONSE pass the null-data check and a
  // 6-byte size check and then copy `len` bytes -- a null dereference and,
  // for len 251..255, an out-of-bounds write. Both were latent (the engines
  // only build REQUESTs), but present.
  const bool has_payload = (fc == FunctionCode::WRITE || pt == PacketType::RESPONSE);
  if (has_payload && !data) {
    optolink_log_w("Function code - data mismatch");
    return false;
  }
  if (id > 7) {
    optolink_log_w("Message id overflow: %u > 7", id);
  }

  // A VS2 payload frame stores its P300 length byte as 0x05 + len, so the
  // payload must keep 0x05 + len inside a single byte: max len == 250.
  if (has_payload && len > 250) {
    optolink_log_w("Payload too large: %u > 250", len);
    return false;
  }
  const std::size_t needed = has_payload ? static_cast<std::size_t>(len) + 6 : 6;
  if (needed > _buffer.size()) {
    optolink_log_e("buffer overflow: need %u > %u", static_cast<unsigned>(needed),
                   static_cast<unsigned>(_buffer.size()));
    return false;
  }

  // Serialize into buffer
  size_t step = 0;
  // 0x05 = standard length: mt, fc, addr(2), len; + data when a payload rides.
  _buffer[step++] = has_payload ? 0x05 + len : 0x05;
  _buffer[step++] = static_cast<uint8_t>(pt);
  _buffer[step++] = static_cast<uint8_t>(fc) | id << 5;
  _buffer[step++] = (addr >> 8) & 0xFF;
  _buffer[step++] = addr & 0xFF;
  _buffer[step++] = len;
  if (has_payload) {
    for (uint8_t i = 0; i < len; ++i) {
      _buffer[step++] = data[i];
    }
  }
  return true;
}

bool PacketVS2::setLength(uint8_t length) {
  if (static_cast<std::size_t>(length) + 1 > _buffer.size()) {
    return false;
  }
  _buffer[0] = length;
  return true;
}

// NOTE: uint8_t arithmetic -- wraps to 0 when the length byte is 255.
// Unreachable today (createPacket caps write payloads at 250 and the raw lane
// at 32), but a live trap if those caps are ever raised.
uint8_t PacketVS2::length() const { return _buffer[0] + 1; }

PacketType PacketVS2::packetType() const { return static_cast<PacketType>(_buffer[1]); }

FunctionCode PacketVS2::functionCode() const { return static_cast<FunctionCode>(_buffer[2] & 0x1F); }

uint16_t PacketVS2::address() const {
  uint16_t retVal = _buffer[3] << 8;
  retVal |= _buffer[4];
  return retVal;
}

uint8_t PacketVS2::dataLength() const { return _buffer[5]; }

const uint8_t *PacketVS2::data() const {
  if (functionCode() == FunctionCode::WRITE)
    return nullptr;
  return &_buffer[6];
}

uint8_t PacketVS2::checksum() const {
  uint8_t retVal = 0;
  for (std::size_t i = 0; i <= _buffer[0]; ++i) {
    retVal += _buffer[i];
  }
  return retVal;
}

void PacketVS2::reset() { _buffer[0] = 0x00; }

}  // namespace esphome::vitohome::optolink
