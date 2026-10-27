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
  // A whole flags byte to accept at FLAGS besides READ/WRITE/RPC, for the
  // answer to a function-code probe (VS2Engine::functionRead()); 0 = none.
  // reset() does not clear it: the engine sets it per request.
  void expectFlags(uint8_t flags);

 private:
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
  uint8_t _expectedFlags;
};

}  // namespace internals
}  // namespace esphome::vitohome::optolink
