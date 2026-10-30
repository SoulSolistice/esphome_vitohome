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

  // Access-mode divergence from upstream (see GWGAccessMode in constants.h):
  // upstream (and vitohome before this change) only ever emitted
  // PacketGWGType.READ (PHYSICAL READ, 0xCB). isKnownGwgReadType() accepts
  // that plus the other seven read-direction TYPE bytes from the openv wiki
  // table, isKnownGwgWriteType() the six write-direction ones; every other
  // byte is rejected here and goes through createTelegram() instead.
  if (!isKnownGwgReadType(packetType) && !isKnownGwgWriteType(packetType)) {
    optolink_log_w("Packet type error: 0x%02x", packetType);
    return false;
  }
  const bool is_write = isKnownGwgWriteType(packetType);
  if (is_write && !data) {
    optolink_log_w("No data for write packet");
    return false;
  }
  return createTelegram(packetType, addr, len, is_write ? data : nullptr);
}

// Any TYPE byte: `01 TYPE ADDR LEN 04`, or with `data` the write layout. The
// frame length and whether a payload rides are fixed here rather than derived
// from the TYPE byte later, so a byte this project has no name for still goes
// out whole (the raw lane, GWGEngine::telegram()).
bool PacketGWG::createTelegram(uint8_t type, uint16_t addr, uint8_t len, const uint8_t *data) {
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

  // bounds check against the fixed buffer (fail-soft, no overflow)
  // write frame = ENQ_ACK + type + addr + len + data[len] + EOT
  const std::size_t needed = data ? static_cast<std::size_t>(len) + 5 : 5;
  // length() and the engine's byte count are uint8_t: a frame of 256 bytes
  // or more would wrap and go out short.
  if (needed > 0xFF) {
    optolink_log_w("frame too long to send: %u bytes", static_cast<unsigned>(needed));
    return false;
  }
  if (needed > _buffer.size()) {
    optolink_log_e("buffer overflow: need %u > %u", static_cast<unsigned>(needed),
                   static_cast<unsigned>(_buffer.size()));
    return false;
  }

  // Serialize into buffer
  size_t step = 0;
  _buffer[step++] = internals::ProtocolBytes.ENQ_ACK;
  _buffer[step++] = type;
  _buffer[step++] = addr & 0xFF;
  _buffer[step++] = len;
  if (data) {
    // Payload placement relative to the EOT terminator is the one part of the
    // GWG write frame with no evidence behind it -- see
    // gwgWriteEotBeforePayload (constants.h) for the two candidate layouts,
    // the evidence for each, and why the default has not moved. Reads are
    // unaffected: both layouts produce the identical 01 TYPE ADDR LEN 04.
    // The flag is read here on every packet build, so flipping it at runtime
    // takes effect on the very next write -- which is what lets one session
    // try both layouts against the same address.
    if (gwgWriteEotBeforePayload) {
      _buffer[step++] = internals::ProtocolBytes.EOT;
      for (uint8_t i = 0; i < len; ++i) {
        _buffer[step++] = data[i];
      }
    } else {
      for (uint8_t i = 0; i < len; ++i) {
        _buffer[step++] = data[i];
      }
      _buffer[step++] = internals::ProtocolBytes.EOT;
    }
  } else {
    _buffer[step++] = internals::ProtocolBytes.EOT;
  }
  // Bug fixed 2026-08-24 (caught by proof_gwg_access_mode.cpp): length() used
  // to compare the TYPE byte against PHYSICAL READ only and reported 0 for the
  // other read modes, so _send() wrote nothing. It is now the length built
  // here, whatever the TYPE byte.
  // uint8_t: createTelegram() refuses a frame above 255 bytes (above).
  _length = static_cast<uint8_t>(step);
  _hasPayload = data != nullptr;
  return true;
}

uint8_t PacketGWG::length() const { return _length; }

bool PacketGWG::hasPayload() const { return _hasPayload; }

uint8_t PacketGWG::packetType() const { return _buffer[1]; }

void PacketGWG::reset() {
  _buffer[3] = 0x00;
  _length = 0;
  _hasPayload = false;
}

}  // namespace esphome::vitohome::optolink
