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

// GWG access mode (divergence from upstream -- upstream has no concept of
// this; PHYSICAL is the only mode it ever emits). Source: openv wiki,
// Protokoll-GWG (https://github.com/openv/openv/wiki/Protokoll-GWG, Dec 2017
// revision, re-fetched 2026-08-24), "Telegramm Typen" table. Read directions
// only -- GWG write access-mode support is deliberately out of scope (see
// GWGEngine::write(), unchanged): the wiki gives write TYPE bytes too, but
// nothing in this project's evidence chain (vcontrold's GWG setaddr is a
// stub, dannerph's write layout disagrees with vitohome's and is untested,
// openv issue #467 shows a live GWG unit only ever returning 0x05 on write)
// makes any GWG write trustworthy enough to extend. PHYSICAL is the
// pre-existing (and only) behaviour -- it stays the default so a config that
// never sets `access:` is unaffected.
enum class GWGAccessMode : uint8_t {
  PHYSICAL = 0,
  VIRTUAL,
  EEPROM,
  XRAM,
  PORT,
  BE,
  KMBUS_RAM,
  KMBUS_EEPROM,
};

// The READ TelegrammByte for each access mode, per the wiki table:
//   VIRTUAL READ=0xC7  PHYSICAL READ=0xCB  EEPROM READ=0xAE
//   PHYSICAL XRAM READ=0xC5  PHYSICAL PORT READ=0x6E  PHYSICAL BE READ=0x9E
//   PHYSICAL KMBUS RAM READ=0x33  PHYSICAL KMBUS EEPROM READ=0x43
// PacketGWGType.READ above is retained as the PHYSICAL case's value (0xCB) so
// existing call sites that reference it directly are unaffected.
constexpr uint8_t gwgReadTypeByte(GWGAccessMode mode) {
  switch (mode) {
    case GWGAccessMode::VIRTUAL:
      return 0xC7;
    case GWGAccessMode::EEPROM:
      return 0xAE;
    case GWGAccessMode::XRAM:
      return 0xC5;
    case GWGAccessMode::PORT:
      return 0x6E;
    case GWGAccessMode::BE:
      return 0x9E;
    case GWGAccessMode::KMBUS_RAM:
      return 0x33;
    case GWGAccessMode::KMBUS_EEPROM:
      return 0x43;
    case GWGAccessMode::PHYSICAL:
    default:
      return 0xCB;
  }
}

// True for any TYPE byte this project supports on the read path -- the
// PHYSICAL READ (0xCB, pre-existing) plus the seven access modes above.
// PacketGWG::createPacket() uses this instead of a single-value comparison so
// a non-PHYSICAL access mode is accepted, while any byte outside this set
// (including every WRITE-direction TYPE byte other than PacketGWGType.WRITE
// itself) is still rejected exactly as before.
constexpr bool isKnownGwgReadType(uint8_t type) {
  return type == 0xCB || type == 0xC7 || type == 0xAE || type == 0xC5 || type == 0x6E || type == 0x9E || type == 0x33 ||
         type == 0x43;
}

// DEVICE_ERROR vs ERROR (divergence from upstream, see THIRD_PARTY.md #9):
// DEVICE_ERROR is a COMPLETE, checksum-valid frame whose type is not RESPONSE
// (e.g. the VS2 device ERROR frame, PacketType 0x03) -- the peer demonstrably
// received the request and answered, so it is proof of a live link speaking
// this protocol. ERROR is malformed traffic (an invalid length/type/function
// code after a start byte) -- possibly line noise -- and proves neither.
// Callers that derive link health from results must not conflate the two.
// PACKET was an upstream parser-progress value that never surfaced as a result
// here and had no references anywhere; removed. CONTINUE is retained: it is
// never produced at runtime, but proof_vs2_guards.cpp uses it as a "no error
// observed yet" sentinel. The live parser-progress enum is ParserResult (below).
enum class OptolinkResult { CONTINUE, TIMEOUT, LENGTH, NACK, CRC, ERROR, DEVICE_ERROR };

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
