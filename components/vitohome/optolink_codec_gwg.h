// Include guards, not #pragma once: see optolink_types.h.
#ifndef VITOHOME_OPTOLINK_CODEC_GWG_H
#define VITOHOME_OPTOLINK_CODEC_GWG_H

// The GWG codec (docs/gwg_protocol.md): the bytes of one
// telegram, without the 01 a link puts in front when it answers an ENQ.
//
//   read     TT AD LEN 04
//   write    TT AD LEN <LEN bytes> 04, or TT AD LEN 04 <LEN bytes> (eot_first)
//   EEPROM   AD AD ~V V 04: one cell, the value and its complement
//
// Addresses are 8 bits. The TYPE byte names the access mode and the direction
// (the tables below). The answer is LEN raw bytes, one acknowledgement byte
// after a write; EEPROM, BE and PORT are read one address per telegram
// (gwgCellLength()), EEPROM cells as the pair ~V V, FF FF where nothing is
// served.

#include <cstdint>
#include <cstring>

#include "optolink_types.h"

namespace esphome::vitohome::optolink {

// The TYPE bytes of the physical read and write.
constexpr struct {
  uint8_t READ = 0xCB;
  uint8_t WRITE = 0xC8;
} PacketGWGType;

// The access modes (openv wiki, Protokoll-GWG, "Telegramm Typen"; the same
// bytes in vcontrold's GWG macros and in speters/vogod). PHYSICAL is the
// default, so a config without `access:` reads physical memory.
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

// RawOp gives its access field no default initializer (RingBuffer needs a
// trivially default-constructible element), so `RawOp operation{}` relies on
// PHYSICAL being 0.
static_assert(static_cast<uint8_t>(GWGAccessMode::PHYSICAL) == 0,
              "GWGAccessMode::PHYSICAL must be 0: RawOp's value-initialized default depends on it");

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

// No write byte: the KM-bus modes are read-only (vogod omits them from its
// writes too). On hardware only the EEPROM write is confirmed.
constexpr uint8_t kNoGwgWriteType = 0x00;

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
      return kNoGwgWriteType;
    case GWGAccessMode::PHYSICAL:
    default:
      return 0xC8;
  }
}

constexpr bool gwgModeIsWritable(GWGAccessMode mode) { return gwgWriteTypeByte(mode) != kNoGwgWriteType; }

// Bytes a unit answers per address in a mode it serves one address per
// telegram, whatever LEN asks for; 0 for a mode that answers LEN bytes.
// EEPROM answers a complement pair, BE and PORT one byte (issue #35,
// GWG_VBEM 0x2053 SW 0x1E).
constexpr uint8_t gwgCellLength(GWGAccessMode mode) {
  switch (mode) {
    case GWGAccessMode::EEPROM:
      return 2;
    case GWGAccessMode::BE:
    case GWGAccessMode::PORT:
      return 1;
    default:
      return 0;
  }
}

constexpr bool gwgReadTypeIsPerAddress(uint8_t type) {
  return type == gwgReadTypeByte(GWGAccessMode::EEPROM) || type == gwgReadTypeByte(GWGAccessMode::BE) ||
         type == gwgReadTypeByte(GWGAccessMode::PORT);
}

// The two sets are disjoint, so a TYPE byte names its direction.
constexpr bool isKnownGwgWriteType(uint8_t type) {
  return type == 0xC8 || type == 0xC4 || type == 0xAD || type == 0xC3 || type == 0x6D || type == 0x9D;
}

constexpr bool isKnownGwgReadType(uint8_t type) {
  return type == 0xCB || type == 0xC7 || type == 0xAE || type == 0xC5 || type == 0x6E || type == 0x9E || type == 0x33 ||
         type == 0x43;
}

// The longest frame on the line: the 01, the header, MAX_PAYLOAD bytes and
// the EOT. An open frame may be that long too.
inline constexpr uint8_t GWG_FRAME_MAX = 5 + MAX_PAYLOAD;
static_assert(GWG_FRAME_MAX <= Telegram::CAPACITY, "a GWG frame must fit a Telegram");

inline constexpr uint8_t GWG_EOT = 0x04;

// The EEPROM write of one cell: AD AD b0 b1 04, b0 the complement of b1 when
// built from a value; a raw pair is sent as given.
inline bool gwg_eeprom_pair(uint16_t address, uint8_t b0, uint8_t b1, Telegram &out) {
  out.length = 0;
  out.has_payload = false;
  out.chain_gap_ms = 0;
  if (address > 0xFF)
    return false;
  uint8_t n = 0;
  out.bytes[n++] = gwgWriteTypeByte(GWGAccessMode::EEPROM);
  out.bytes[n++] = static_cast<uint8_t>(address);
  out.bytes[n++] = b0;
  out.bytes[n++] = b1;
  out.bytes[n++] = GWG_EOT;
  out.length = n;
  out.has_payload = true;
  return true;
}

// Any TYPE byte, as given. False for a zero length, an address past 0xFF, a
// payload past MAX_PAYLOAD, or an EEPROM write of more than one byte.
inline bool gwg_encode(uint8_t type, uint16_t address, uint8_t length, const uint8_t *payload, bool eot_first,
                       Telegram &out) {
  out.length = 0;
  out.has_payload = false;
  out.chain_gap_ms = 0;
  if (length == 0 || address > 0xFF)
    return false;
  if (1u + (payload != nullptr ? 4u + length : 4u) > GWG_FRAME_MAX)
    return false;
  if (payload != nullptr && type == gwgWriteTypeByte(GWGAccessMode::EEPROM)) {
    if (length != 1)
      return false;
    return gwg_eeprom_pair(address, static_cast<uint8_t>(~payload[0]), payload[0], out);
  }
  uint8_t n = 0;
  out.bytes[n++] = type;
  out.bytes[n++] = static_cast<uint8_t>(address);
  out.bytes[n++] = length;
  if (payload != nullptr) {
    if (eot_first)
      out.bytes[n++] = GWG_EOT;
    std::memcpy(&out.bytes[n], payload, length);
    n = static_cast<uint8_t>(n + length);
    if (!eot_first)
      out.bytes[n++] = GWG_EOT;
  } else {
    out.bytes[n++] = GWG_EOT;
  }
  out.length = n;
  out.has_payload = payload != nullptr;
  return true;
}

// A read or write of a known access mode: a write needs a payload, a read
// takes none.
inline bool gwg_encode_mode(uint8_t type, uint16_t address, uint8_t length, const uint8_t *payload, bool eot_first,
                            Telegram &out) {
  out.length = 0;
  out.has_payload = false;
  if (!isKnownGwgReadType(type) && !isKnownGwgWriteType(type))
    return false;
  const bool write = isKnownGwgWriteType(type);
  if (write && payload == nullptr)
    return false;
  return gwg_encode(type, address, length, write ? payload : nullptr, eot_first, out);
}

}  // namespace esphome::vitohome::optolink

#endif  // VITOHOME_OPTOLINK_CODEC_GWG_H
