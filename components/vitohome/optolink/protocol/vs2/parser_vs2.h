/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#pragma once

#include <cstddef>
#include <cstdint>

#include "../../constants.h"
#include "../../helpers.h"
#include "../../logging.h"
#include "packet_vs2.h"

namespace esphome::vitohome::optolink {
namespace internals {

class ParserVS2 {
 public:
  ParserVS2();
  ParserResult parse(const uint8_t b);
  const PacketVS2 &packet() const;
  void reset();
  // Accept a frame with this function code too (0: none beyond READ / WRITE /
  // RPC), for the request in flight (VS2Engine::request()). Its payload is
  // taken from the frame length, as for an RPC answer. Survives reset(): it
  // belongs to the request, not to one frame.
  void expectFunction(uint8_t function);

 private:
  static bool isNamedFunction(FunctionCode function);
  uint8_t _expectedFunction{0};
  PacketVS2 _packet;
  enum class ParserStep {
    STARTBYTE,
    PACKETLENGTH,
    PACKETTYPE,
    FLAGS,
    ADDRESS1,
    ADDRESS2,
    PAYLOADLENGTH,
    PAYLOAD,
    CHECKSUM
  } _step;
  uint8_t _payloadLength;
};

}  // namespace internals
}  // namespace esphome::vitohome::optolink
