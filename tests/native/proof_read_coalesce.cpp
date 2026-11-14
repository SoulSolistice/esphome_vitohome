// Proves VitoEntityBase::reads_same_datapoint(), the test the hub's read
// coalescing rests on (vitohome.cpp, share_read_response_): when one entity's
// poll read completes, every other entity waiting for THE SAME TELEGRAM is
// handed the same outcome instead of sending its own.
//
// "The same telegram" must be exactly that. Too wide, and an entity publishes
// bytes read from another register -- on GWG an address means a different
// datapoint in every access mode, and a P300 remote procedure call returns a
// different list entry for every parameter. Too narrow only wastes telegrams.
//
// Built once per protocol by build_and_run_protocols.sh: the access spaces an
// entity can be in differ by protocol.
#include <cstdio>
#include <vector>

#include "vito_entity.h"

using namespace esphome::vitohome;

namespace {

// The smallest concrete entity that can carry a write target.
class Probe final : public VitoWritableEntity {
 public:
  Probe(uint16_t address, uint8_t length) {
    this->set_datapoint(optolink::Datapoint("probe", address, length, optolink::noconv));
  }
  void handle_response(const ResponseView &) override {}
  void handle_error(optolink::OptolinkResult) override {}
  const char *entity_kind() const override { return "probe"; }
  void dump_config() override {}
};

int g_fail = 0;
void check(bool ok, const char *what) {
  std::printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    g_fail++;
}

}  // namespace

int main() {
  std::printf("== read coalescing: which entities share one telegram (%s) ==\n", PROTOCOL_NAME);

  Probe a(0x0055, 1);
  Probe b(0x0055, 1);
  check(a.reads_same_datapoint(b) && b.reads_same_datapoint(a), "same address and length: shared");
  check(a.reads_same_datapoint(a), "an entity and itself");

  // What an entity takes FROM the answer does not make it another telegram:
  // eight coding flags of one byte are one read.
  b.set_field_mask(0x08);
  b.set_poll_interval(3600000);
  check(a.reads_same_datapoint(b), "a bit mask and an interval do not matter");

  Probe other_address(0x0056, 1);
  check(!a.reads_same_datapoint(other_address), "another address: not shared");

  // A 1-byte read and a 2-byte read of the same address are two telegrams,
  // and the shorter answer would not satisfy the longer entity.
  Probe longer(0x0055, 2);
  check(!a.reads_same_datapoint(longer) && !longer.reads_same_datapoint(a), "another length: not shared");

  // A separate write target is not part of the read.
  Probe command(0x0055, 1);
  command.set_write_datapoint(optolink::Datapoint("probe", 0x2323, 1, optolink::noconv));
  check(a.reads_same_datapoint(command), "a different write address does not matter");

#if defined(VITOHOME_PROTOCOL_GWG)
  // GWG: the address space is per access mode. 0x01 in eeprom and 0x01 in
  // port are two registers.
  Probe eeprom(0x0001, 1);
  Probe eeprom2(0x0001, 1);
  Probe port(0x0001, 1);
  Probe physical(0x0001, 1);
  eeprom.set_access(optolink::GWGAccessMode::EEPROM);
  eeprom2.set_access(optolink::GWGAccessMode::EEPROM);
  port.set_access(optolink::GWGAccessMode::PORT);
  check(eeprom.reads_same_datapoint(eeprom2), "GWG: same mode and address: shared");
  check(!eeprom.reads_same_datapoint(port), "GWG: eeprom 0x01 and port 0x01: not shared");
  check(!eeprom.reads_same_datapoint(physical), "GWG: eeprom 0x01 and physical 0x01: not shared");
#else
  // KW and P300: the burner control's address space is its own. 0x4050 there
  // and 0x4050 in the Vitotronic are two datapoints.
  Probe own(0x4050, 1);
  Probe gfa(0x4050, 1);
  Probe gfa2(0x4050, 1);
  gfa.set_gfa();
  gfa2.set_gfa();
  check(gfa.reads_same_datapoint(gfa2), "GFA: same address: shared");
  check(!own.reads_same_datapoint(gfa) && !gfa.reads_same_datapoint(own), "GFA 0x4050 and own 0x4050: not shared");
#endif

#if !defined(VITOHOME_PROTOCOL_GWG) && !defined(VITOHOME_PROTOCOL_KW)
  // P300 remote procedure call: the parameter picks the list entry, so two
  // entities at one address share only with the same parameter.
  Probe rpc_a(0xA050, 4);
  Probe rpc_b(0xA050, 4);
  Probe rpc_c(0xA050, 4);
  Probe rpc_none(0xA050, 4);
  Probe rpc_short(0xA050, 4);
  Probe plain(0xA050, 4);
  rpc_a.set_rpc_prefix(std::vector<uint8_t>{0x01, 0x02});
  rpc_b.set_rpc_prefix(std::vector<uint8_t>{0x01, 0x02});
  rpc_c.set_rpc_prefix(std::vector<uint8_t>{0x01, 0x03});
  rpc_none.set_rpc_prefix(std::vector<uint8_t>{});
  rpc_short.set_rpc_prefix(std::vector<uint8_t>{0x01});
  check(rpc_a.reads_same_datapoint(rpc_b), "RPC: same address and parameter: shared");
  check(!rpc_a.reads_same_datapoint(rpc_c), "RPC: another parameter: not shared");
  check(!rpc_a.reads_same_datapoint(rpc_short) && !rpc_short.reads_same_datapoint(rpc_a),
        "RPC: a parameter and its own prefix: not shared");
  check(!rpc_a.reads_same_datapoint(rpc_none), "RPC: with and without a parameter: not shared");
  check(!rpc_none.reads_same_datapoint(plain), "RPC without a parameter and a plain read: not shared");
#endif

  std::printf("read coalescing proof: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
