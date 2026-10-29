#pragma once

#include <cstdint>
#include <type_traits>

#include "optolink/optolink.h"
#include "protocol_select.h"

namespace esphome::vitohome {

// Where a datapoint lives, and so which telegram reads (and writes) it -- the
// YAML `access:` of an entity, Vitosoft's FCRead of its row. One per entity
// (VitoEntityBase::space()) and per raw-lane operation (RawOp::space); the hub's
// send_read_() / send_write_() (vitohome.cpp) are the only places it becomes a
// telegram, and the only places that know which engine call that is.
//
// Adding an access function means: an enumerator here, its rows in
// space_available() / space_writable() / space_name() (and
// space_has_switch_cost() if reaching it costs a protocol switch), its branch
// in send_read_() / send_write_(), and its YAML value in ACCESS_SPACES
// (__init__.py). Nothing else in the hub dispatches on it.
enum class AddressSpace : uint8_t {
  // The protocol's own read and write: KW F7/F4, P300 function codes 01/02, GWG
  // PHYSICAL (0xCB/0xC8). Zero, so a value-initialized RawOp and an entity that
  // never sets `access:` read as they always did. Not called OWN: the
  // ESP8266 Arduino core defines that as a macro.
  OWN = 0,
  // The burner control (Vitosoft GFA_READ): the KW telegram 0x6B. Read-only.
  // Over P300 the engine detours to KW for it (VS2Engine::gfaRead()).
  GFA,
  // A P300 remote procedure call (function code 0x07), with the entity's
  // `prefix:` as its parameter. Read-only as a datapoint; an action button
  // sends it on the write lane for its effect.
  RPC,
  // GWG's other access modes (GWGAccessMode; PHYSICAL is OWN).
  GWG_VIRTUAL,
  GWG_EEPROM,
  GWG_XRAM,
  GWG_PORT,
  GWG_BE,
  GWG_KMBUS_RAM,
  GWG_KMBUS_EEPROM,
};

// Enforced, not merely documented: VitoHomeComponent::RawOp relies on
// value-initialization for its space (see there), which is OWN only while
// OWN is the zero enumerator.
static_assert(static_cast<uint8_t>(AddressSpace::OWN) == 0, "RawOp's value-initialized space depends on it");

inline constexpr bool IS_P300 = std::is_same<SelectedProtocol, optolink::P300>::value;
inline constexpr bool IS_KW = std::is_same<SelectedProtocol, optolink::KW>::value;
inline constexpr bool IS_GWG = std::is_same<SelectedProtocol, optolink::GWG>::value;

constexpr bool is_gwg_space(AddressSpace space) {
  return space >= AddressSpace::GWG_VIRTUAL && space <= AddressSpace::GWG_KMBUS_EEPROM;
}

// Whether the selected protocol has a telegram for the space. The config
// validation rejects the others (__init__.py, ACCESS_SPACES); send_read_()
// refuses them permanently all the same.
constexpr bool space_available(AddressSpace space) {
  switch (space) {
    case AddressSpace::OWN:
      return true;
    case AddressSpace::GFA:
      return IS_KW || IS_P300;
    case AddressSpace::RPC:
      return IS_P300;
    default:
      return IS_GWG && is_gwg_space(space);
  }
}

// Whether the space has a write telegram. GFA has none (Vitosoft defines no GFA
// write), neither have GWG's KMBUS modes; RPC writes nothing either -- an action
// button's call is a call, not a write of data.
constexpr bool space_writable(AddressSpace space) {
  switch (space) {
    case AddressSpace::GFA:
    case AddressSpace::RPC:
    case AddressSpace::GWG_KMBUS_RAM:
    case AddressSpace::GWG_KMBUS_EEPROM:
      return false;
    default:
      return space_available(space);
  }
}

// Whether reaching the space costs a protocol switch, so that reads in it are
// best dispatched back to back (the hub's address-space affinity): over P300 a
// GFA read leaves the link for KW and returns, up to two ENQ waits.
constexpr bool space_has_switch_cost(AddressSpace space) { return IS_P300 && space == AddressSpace::GFA; }

// How many telegrams a read of `length` bytes in `space` takes on the wire. A
// GWG EEPROM read is answered as a complement pair per address, so the engine
// reads one address per telegram (GWGEngine::read(), kEepromPairLength) and
// re-arms its own timeout for each; after a burst fallback each one waits for
// an ENQ. Every other read is one telegram.
constexpr uint8_t read_telegram_count(AddressSpace space, uint8_t length) {
  return (IS_GWG && space == AddressSpace::GWG_EEPROM && length > 1) ? length : 1;
}

// The hub's last-resort watchdog for one operation: `per_telegram_ms` for each
// telegram it takes. A single budget for a whole multi-telegram GWG EEPROM read
// cut off a valid 16-byte read after burst fallback (about 19 s at 1.2 s per
// ENQ) while the engine was still progressing.
constexpr uint32_t in_flight_watchdog_ms(uint32_t per_telegram_ms, uint8_t telegrams) {
  return per_telegram_ms * (telegrams == 0 ? 1u : telegrams);
}

// GWG's access mode for a space: PHYSICAL for OWN (and anything that is no
// GWG mode, which never reaches a GWG engine -- see space_available()).
constexpr optolink::GWGAccessMode gwg_mode(AddressSpace space) {
  switch (space) {
    case AddressSpace::GWG_VIRTUAL:
      return optolink::GWGAccessMode::VIRTUAL;
    case AddressSpace::GWG_EEPROM:
      return optolink::GWGAccessMode::EEPROM;
    case AddressSpace::GWG_XRAM:
      return optolink::GWGAccessMode::XRAM;
    case AddressSpace::GWG_PORT:
      return optolink::GWGAccessMode::PORT;
    case AddressSpace::GWG_BE:
      return optolink::GWGAccessMode::BE;
    case AddressSpace::GWG_KMBUS_RAM:
      return optolink::GWGAccessMode::KMBUS_RAM;
    case AddressSpace::GWG_KMBUS_EEPROM:
      return optolink::GWGAccessMode::KMBUS_EEPROM;
    default:
      return optolink::GWGAccessMode::PHYSICAL;
  }
}

// The space of a GWG access mode. On any other protocol a GWG mode has no
// meaning and maps to OWN: the raw-lane API takes a GWGAccessMode argument
// on every build (so lambdas need no #ifdef), and it has always been ignored
// outside GWG.
constexpr AddressSpace space_from_gwg(optolink::GWGAccessMode mode) {
  if (!IS_GWG)
    return AddressSpace::OWN;
  switch (mode) {
    case optolink::GWGAccessMode::VIRTUAL:
      return AddressSpace::GWG_VIRTUAL;
    case optolink::GWGAccessMode::EEPROM:
      return AddressSpace::GWG_EEPROM;
    case optolink::GWGAccessMode::XRAM:
      return AddressSpace::GWG_XRAM;
    case optolink::GWGAccessMode::PORT:
      return AddressSpace::GWG_PORT;
    case optolink::GWGAccessMode::BE:
      return AddressSpace::GWG_BE;
    case optolink::GWGAccessMode::KMBUS_RAM:
      return AddressSpace::GWG_KMBUS_RAM;
    case optolink::GWGAccessMode::KMBUS_EEPROM:
      return AddressSpace::GWG_KMBUS_EEPROM;
    case optolink::GWGAccessMode::PHYSICAL:
    default:
      return AddressSpace::OWN;
  }
}

// For logs: the YAML `access:` value ("" for OWN).
constexpr const char *space_name(AddressSpace space) {
  switch (space) {
    case AddressSpace::GFA:
      return "gfa";
    case AddressSpace::RPC:
      return "rpc";
    case AddressSpace::GWG_VIRTUAL:
      return "virtual";
    case AddressSpace::GWG_EEPROM:
      return "eeprom";
    case AddressSpace::GWG_XRAM:
      return "xram";
    case AddressSpace::GWG_PORT:
      return "port";
    case AddressSpace::GWG_BE:
      return "be";
    case AddressSpace::GWG_KMBUS_RAM:
      return "kmbus_ram";
    case AddressSpace::GWG_KMBUS_EEPROM:
      return "kmbus_eeprom";
    case AddressSpace::OWN:
    default:
      return "";
  }
}

}  // namespace esphome::vitohome
