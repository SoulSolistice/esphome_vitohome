/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#include "parser_vs2.h"

namespace esphome {
namespace vitohome {
namespace optolink {
namespace internals {

ParserVS2::ParserVS2() : _packet(), _step(ParserStep::STARTBYTE), _payloadLength(0) {
  // empty
}

ParserResult ParserVS2::parse(const uint8_t b) {
  switch (_step) {
    case ParserStep::STARTBYTE:
      if (b != ProtocolBytes.PACKETSTART) {
        optolink_log_w("Invalid packet start: 0x%02x", b);
        break;
      }
      _packet.reset();
      _step = ParserStep::PACKETLENGTH;
      break;

    case ParserStep::PACKETLENGTH:
      if (b < 5) {
        optolink_log_w("Invalid packet length: %u", b);
        _step = ParserStep::STARTBYTE;
        return ParserResult::ERROR;
      }
      if (!_packet.setLength(b)) {
        optolink_log_e("Could not parse packet");
        _step = ParserStep::STARTBYTE;
        return ParserResult::ERROR;
      }
      _step = ParserStep::PACKETTYPE;
      break;

    case ParserStep::PACKETTYPE:
      if (b > 0x03) {
        optolink_log_w("Invalid packet type: 0x%02x", b);
        _step = ParserStep::STARTBYTE;
        return ParserResult::ERROR;
      }
      _packet[1] = b;
      _step = ParserStep::FLAGS;
      break;

    case ParserStep::FLAGS: {
      uint8_t fc = b & 0x1F;
      if (fc != 0x01 && fc != 0x02 && fc != 0x07) {
        optolink_log_w("Invalid packet fc: 0x%02x", fc);
        _step = ParserStep::STARTBYTE;
        return ParserResult::ERROR;
      }
    }
      _packet[2] = b;
      _step = ParserStep::ADDRESS1;
      break;

    case ParserStep::ADDRESS1:
      _packet[3] = b;
      _step = ParserStep::ADDRESS2;
      break;

    case ParserStep::ADDRESS2:
      _packet[4] = b;
      _step = ParserStep::PAYLOADLENGTH;
      break;

    case ParserStep::PAYLOADLENGTH:
      _packet[5] = b;
      if ((_packet.functionCode() == FunctionCode::READ && _packet.packetType() == PacketType::REQUEST) ||
          (_packet.functionCode() == FunctionCode::WRITE && _packet.packetType() == PacketType::RESPONSE)) {
        // read requests and write responses don't have a data payload
        _step = ParserStep::CHECKSUM;
      } else {
        if (b != _packet.length() - 6U) {
          optolink_log_w("Invalid payload length: %u (expected %u)", b, _packet.length() - 6U);
          _step = ParserStep::STARTBYTE;
          return ParserResult::ERROR;
        }
        _payloadLength = b;
        _step = ParserStep::PAYLOAD;
      }
      break;

    case ParserStep::PAYLOAD:
      _packet[6 + _packet.dataLength() - _payloadLength--] = b;
      if (_payloadLength == 0) {
        _step = ParserStep::CHECKSUM;
      }
      break;

    case ParserStep::CHECKSUM:
      if (_packet.checksum() != b) {
        optolink_log_w("Invalid checksum: 0x%02x (calculated 0x%02x)", b, _packet.checksum());
        _step = ParserStep::STARTBYTE;
        return ParserResult::CS_ERROR;
      }
      _step = ParserStep::STARTBYTE;
      return ParserResult::COMPLETE;
  }
  return ParserResult::CONTINUE;
}

const PacketVS2& ParserVS2::packet() const { return _packet; }

void ParserVS2::reset() {
  _packet.reset();
  _step = ParserStep::STARTBYTE;
  _payloadLength = 0;
}

}  // namespace internals
}  // namespace optolink
}  // namespace vitohome
}  // namespace esphome
