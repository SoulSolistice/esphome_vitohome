// The address-space table (address_space.h) per protocol: which `access:`
// spaces have a telegram, which are written, which cost a protocol switch, and
// the GWG mode mapping. Compiled once per protocol by
// build_and_run_protocols.sh; everything is a static_assert, so a wrong row
// fails the build.
#include <cstdio>

#include "address_space.h"

using esphome::vitohome::AddressSpace;
using namespace esphome::vitohome;
namespace optolink = esphome::vitohome::optolink;

// OWN is the protocol's own read and write everywhere.
static_assert(space_available(AddressSpace::OWN) && space_writable(AddressSpace::OWN), "OWN");

// GFA: KW and P300, read-only; a switch only over P300.
static_assert(space_available(AddressSpace::GFA) == (IS_KW || IS_P300), "GFA availability");
static_assert(!space_writable(AddressSpace::GFA), "no GFA write");
static_assert(space_has_switch_cost(AddressSpace::GFA) == IS_P300, "GFA switch cost");

// RPC: P300 only, never a write of data.
static_assert(space_available(AddressSpace::RPC) == IS_P300, "RPC availability");
static_assert(!space_writable(AddressSpace::RPC), "no RPC write");

// GWG modes: GWG only; KMBUS read-only.
static_assert(space_available(AddressSpace::GWG_EEPROM) == IS_GWG, "GWG mode availability");
static_assert(space_writable(AddressSpace::GWG_EEPROM) == IS_GWG, "EEPROM writable on GWG");
static_assert(!space_writable(AddressSpace::GWG_KMBUS_RAM) && !space_writable(AddressSpace::GWG_KMBUS_EEPROM),
              "KMBUS read-only");
static_assert(!space_has_switch_cost(AddressSpace::GWG_EEPROM), "no GWG switch cost");

// The mapping round-trips on GWG and collapses to OWN elsewhere (the
// raw-lane API takes a GWGAccessMode on every build).
static_assert(gwg_mode(AddressSpace::OWN) == optolink::GWGAccessMode::PHYSICAL, "OWN is PHYSICAL");
static_assert(space_from_gwg(optolink::GWGAccessMode::PHYSICAL) == AddressSpace::OWN, "PHYSICAL is OWN");
static_assert(IS_GWG ? gwg_mode(space_from_gwg(optolink::GWGAccessMode::BE)) == optolink::GWGAccessMode::BE
                     : space_from_gwg(optolink::GWGAccessMode::BE) == AddressSpace::OWN,
              "GWG modes map only on GWG");

// The hub watchdog's telegram count: a GWG EEPROM read takes one telegram per
// address, everything else one.
static_assert(read_telegram_count(AddressSpace::GWG_EEPROM, 16) == (IS_GWG ? 16 : 1), "EEPROM per address");
static_assert(read_telegram_count(AddressSpace::GWG_EEPROM, 1) == 1, "one address, one telegram");
static_assert(read_telegram_count(AddressSpace::OWN, 48) == 1, "an own read is one telegram");
static_assert(read_telegram_count(AddressSpace::GWG_XRAM, 8) == 1, "an XRAM read is one telegram");
static_assert(in_flight_watchdog_ms(10000, 16) == 160000, "budget per telegram");
static_assert(in_flight_watchdog_ms(10000, 0) == 10000, "at least one telegram");
// 48 bytes (RAW_READ_MAX) at 10 s each stays far inside uint32_t.
static_assert(in_flight_watchdog_ms(10000, 48) == 480000, "no overflow at the raw cap");

int main() {
  std::printf("  %-5s address-space table ok\n", PROTOCOL_NAME);
  return 0;
}
