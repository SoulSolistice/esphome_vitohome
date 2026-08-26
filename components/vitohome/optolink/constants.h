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
// this; PHYSICAL is the only mode it ever emits). Primary source: openv wiki,
// Protokoll-GWG (https://github.com/openv/openv/wiki/Protokoll-GWG, Dec 2017
// revision, re-fetched 2026-08-24), "Telegramm Typen" table.
//
// Corroborated against three independent implementations. vcontrold
// (openv/vcontrold,
// xml/{kw,300}/vcontrold.xml, <protocol name="GWG">), which defines the same
// TYPE bytes as named macros and wraps them in the SAME frame this engine
// emits -- `SYNC; GET*ADDR $addr $hexlen 04; RECV $len`, i.e. 01 <TYPE> <addr>
// <len> 04 answered by exactly <len> raw bytes. Seven of the eight match
// byte-for-byte:
//
//   PHYSICAL 0xCB = GETADDR    VIRTUAL 0xC7 = GETVADDR   EEPROM 0xAE = GETEADDR
//   XRAM     0xC5 = GETXADDR   PORT    0x6E = GETPADDR   BE     0x9E = GETBADDR
//   KMBUS_EEPROM 0x43 = GETKMADDR
//
// KMBUS_RAM (0x33) has no vcontrold macro and no Vitosoft datapoint uses it,
// but it is NOT single-sourced: speters/vogod declares the full GWG
// command-type set independently (pkg/vogo/fsm.go), physicalKmbusRAMRead =
// 0x33 included. Two sources agree on the byte; what is missing is any
// datapoint that uses it.
//
// vcontrold also ships datapoints that actually USE four of the non-physical
// modes for the one GWG device it knows (ID 2053, GWG_VBEM) -- getDevType
// reads 0xF8 len 4 over VIRTUAL, getBrennerStunden1 reads 0x17 len 2 over
// EEPROM, getVentilStatus/getPumpeStatus* read 0x01 over PORT, getExtBA reads
// 0x00 over XRAM -- so these are not merely tabulated, they are what a
// long-lived implementation talks to real GWG units with. (0x43 is the
// exception on that front too: its getkmaddr command references an undefined
// `GETKMDDR` macro -- a typo in vcontrold -- so that path is dead there.)
//
// Read directions only -- GWG write access-mode support is deliberately out of
// scope (see GWGEngine::write(), unchanged): the wiki gives write TYPE bytes
// too, but no implementation anywhere has been confirmed to write to a GWG
// unit. vogod declares the write bytes but never builds a GWG frame -- its
// prepareCmd() handles only the P300 and KW send states and returns
// "not implemented ... (GWG protocol?)" for every GWG type -- so it does not
// settle the frame layout either. vcontrold's GWG `setaddr` is not merely a stub
// but a no-op -- its whole body is `SYNC;RECV 1`, which sends the sync EOT and
// reads a byte without ever transmitting an address, a length, a payload or a
// TYPE byte. dannerph's write layout disagrees with vitohome's and is
// untested; openv issue #467 shows a live GWG unit only ever returning 0x05 on
// write.
//
// PHYSICAL is the pre-existing (and only) behaviour -- it stays the default so
// a config that never sets `access:` is unaffected.
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

// The WRITE TelegrammByte for each access mode, per the same wiki table:
//   VIRTUAL WRITE=0xC4  PHYSICAL WRITE=0xC8  EEPROM WRITE=0xAD
//   PHYSICAL XRAM WRITE=0xC3  PHYSICAL PORT WRITE=0x6D  PHYSICAL BE WRITE=0x9D
// The wiki lists NO write byte for either KMBUS mode, so those two are
// read-only here and this returns kNoGwgWriteType for them; callers must check.
// PacketGWGType.WRITE above is retained as the PHYSICAL case's value (0xC8).
//
// The write mode always mirrors the read mode -- Vitosoft pairs every writable
// GWG datapoint as (EEPROM_READ, EEPROM_WRITE) or (BE_READ, BE_WRITE), never
// across modes -- so one `access:` selects both directions.
//
// Which modes are writable in practice, per Vitosoft across all 22 GWG device
// tokens: EEPROM_WRITE 1082 datapoints, BE_WRITE 280, and **zero**
// PHYSICAL_WRITE. That matters, because 0xC8 is the only write byte this
// engine could emit before this table existed -- and openv issue #467's "GWG
// only ever answers 0x05 on write" is a physical-mode write against a unit
// where nothing is physical-writable, with the next idle ENQ misread as the
// ack (see RESPONSE_TIMEOUT_MS). Cross-checked against
// dannerph/esphome_vitoconnect, which implements the same six write bytes, and
// speters/vogod, whose CommandType enum lists exactly these six writes and
// deliberately omits both KMBUS modes from its writeCmds map -- i.e. it reaches
// the same read-only conclusion for KMBUS that gwgModeIsWritable() encodes.
constexpr uint8_t kNoGwgWriteType = 0x00;

// GWG WRITE frame layout switch. The read frame is settled -- three sources
// agree on `01 TYPE ADDR LEN 04` -- but where the payload sits relative to the
// EOT terminator on a WRITE is genuinely unknown, and the two existing
// implementations disagree:
//
//   false (default, this project's pre-existing layout):
//       01 TYPE ADDR LEN <data...> 04     EOT terminates the frame
//   true (dannerph/esphome_vitoconnect):
//       01 TYPE ADDR LEN 04 <data...>     EOT precedes the payload
//
// Neither has hardware evidence. The wiki calls 0x04 the "Telegramm Ende Byte"
// and shows no write example, which argues for the default; the KW sibling
// (vcontrold KW2 `SETADDR $addr $hexlen; SEND BYTES`) emits no terminator at
// all, so it does not decide the question either. vcontrold has no GWG write
// implementation to compare against -- its `setaddr` is `SYNC;RECV 1`, which
// transmits no telegram whatsoever.
//
// Flip this to true to test the other layout. Reads are byte-identical under
// both. Once a unit settles it, this switch and this comment should collapse
// into the single correct layout.
constexpr bool kGwgWriteEotBeforePayload = false;

constexpr uint8_t gwgWriteTypeByte(GWGAccessMode mode) {
  switch (mode) {
    case GWGAccessMode::VIRTUAL:
      return 0xC4;
    case GWGAccessMode::EEPROM:
      return 0xAD;
    case GWGAccessMode::XRAM:
      return 0xC3;
    case GWGAccessMode::PORT:
      return 0x6D;
    case GWGAccessMode::BE:
      return 0x9D;
    case GWGAccessMode::KMBUS_RAM:
    case GWGAccessMode::KMBUS_EEPROM:
      return kNoGwgWriteType;  // no write byte exists for these
    case GWGAccessMode::PHYSICAL:
    default:
      return 0xC8;
  }
}

constexpr bool gwgModeIsWritable(GWGAccessMode mode) { return gwgWriteTypeByte(mode) != kNoGwgWriteType; }

// True for any TYPE byte this project supports on the write path. Mirrors
// isKnownGwgReadType() below; the two sets are disjoint, so a TYPE byte
// unambiguously names its direction.
constexpr bool isKnownGwgWriteType(uint8_t type) {
  return type == 0xC8 || type == 0xC4 || type == 0xAD || type == 0xC3 || type == 0x6D || type == 0x9D;
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
