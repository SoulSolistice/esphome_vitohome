/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::vitohome::optolink {

enum class FunctionCode : uint8_t {
  READ = 0x01,
  WRITE = 0x02,
  RPC = 0x07,
};

enum class PacketType : uint8_t {
  REQUEST = 0x00,
  RESPONSE = 0x01,
  UNACKED = 0x02,
  ERROR = 0x03,
};

constexpr struct {
  uint8_t READ = 0xF7;
  uint8_t WRITE = 0xF4;
} PacketVS1Type;

constexpr struct {
  uint8_t READ = 0xCB;
  uint8_t WRITE = 0xC8;
} PacketGWGType;

// DEVICE_ERROR vs ERROR (divergence from upstream, see THIRD_PARTY.md #9):
// DEVICE_ERROR is a COMPLETE, checksum-valid frame whose type is not RESPONSE
// (e.g. the VS2 device ERROR frame, PacketType 0x03) -- the peer demonstrably
// received the request and answered, so it is proof of a live link speaking
// this protocol. ERROR is malformed traffic (an invalid length/type/function
// code after a start byte) -- possibly line noise -- and proves neither.
// Callers that derive link health from results must not conflate the two.
enum class OptolinkResult { CONTINUE, PACKET, TIMEOUT, LENGTH, NACK, CRC, ERROR, DEVICE_ERROR };

const char *errorToString(OptolinkResult error);

}  // namespace esphome::vitohome::optolink

namespace esphome::vitohome::optolink {
namespace internals {

constexpr struct {
  uint8_t PACKETSTART = 0x41;
  uint8_t ACK = 0x06;
  uint8_t ENQ_ACK = 0x01;
  uint8_t NACK = 0x15;
  uint8_t ENQ = 0x05;
  uint8_t EOT = 0x04;
  uint8_t SYNC[3] = {0x16, 0x00, 0x00};
} ProtocolBytes;

enum class ParserResult { CONTINUE, COMPLETE, CS_ERROR, ERROR };

}  // namespace internals
}  // namespace esphome::vitohome::optolink
