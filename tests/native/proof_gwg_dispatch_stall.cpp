// Proves the transient-vs-permanent refusal distinction the hub's dispatch
// lanes rely on (vitohome.cpp dispatch_next_ / dispatch_raw_front_).
//
// read()/write() returning false is overloaded. A read/write of a 16-bit
// address on GWG is a PERMANENT createPacket() rejection (PacketGWG rejects
// addr > 0xFF): read() returns false AND isBusy() stays false, and nothing is
// written to the wire, so a false return means "will never work", not "retry".
// A refusal while a transaction is in flight is TRANSIENT: read() returns
// false but isBusy() is true, so the hub must retain-and-retry.
//
// This is the invariant that makes isBusy() a correct permanent/transient
// discriminator. Without it the raw lane retains a permanently-refused op at
// its head and re-attempts it every loop tick, and because dispatch_next_()
// services the raw queue before the read/write lanes and returns, ALL bus
// traffic (polling, user writes, clock sync) stalls silently until reboot --
// with nothing in flight for IN_FLIGHT_WATCHDOG_MS to catch. A GWG raw op with
// a 16-bit address is the one live trigger past the schema/RAW_*_MAX caps.
//
// Built with -DVITOHOME_PROTOCOL_GWG by build_and_run_protocols.sh.
#include <cstdio>

#include "fake_optolink.h"
#include "protocol_select.h"

using namespace esphome::vitohome;
using Engine = optolink::OptolinkEngine<SelectedProtocol>;

int main() {
  FakeOptolink uart;
  Engine adapter(&uart);  // GWGEngine under -DVITOHOME_PROTOCOL_GWG
  adapter.onResponse([](const uint8_t *, uint8_t, uint16_t) {});
  adapter.onError([](optolink::OptolinkResult, uint16_t) {});
  adapter.begin();

  int failures = 0;
  auto check = [&failures](bool ok, const char *what) {
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
      failures++;
  };

  // --- PERMANENT: a 16-bit address is rejected by createPacket() -----------
  check(!adapter.read(0x0800, 2), "read(0x0800,2) refused (addr > 0xFF)");
  check(!adapter.isBusy(), "engine NOT busy after permanent refusal");
  check(uart.written().empty(), "nothing written to the wire on refusal");

  // Retrying the same op forever must never accept it and never go busy: this
  // is precisely the hub's old retain-and-spin, proven non-progressing so the
  // permanent branch (drop) is the only escape.
  bool ever_accepted = false;
  bool ever_busy = false;
  for (int i = 0; i < 1000; ++i) {
    if (adapter.read(0x0800, 2))
      ever_accepted = true;
    if (adapter.isBusy())
      ever_busy = true;
    adapter.loop();
  }
  check(!ever_accepted, "1000x retry never accepts the op");
  check(!ever_busy, "1000x retry never goes busy");
  check(uart.written().empty(), "1000x retry writes zero bytes to the wire");

  // Same for a write of a 16-bit address (raw write path).
  const uint8_t wr[1] = {0x07};
  check(!adapter.write(0x0900, wr, 1), "write(0x0900,..) refused (addr > 0xFF)");
  check(!adapter.isBusy(), "engine NOT busy after permanent write refusal");

  // --- TRANSIENT: a refusal while busy must report isBusy() == true --------
  // A valid 8-bit address is accepted; the engine is now busy. A second read is
  // refused, but isBusy() is true -> the hub must RETAIN (retry), not drop.
  check(adapter.read(0x0055, 2), "read(0x0055,2) accepted (addr <= 0xFF)");
  check(adapter.isBusy(), "engine busy after an accepted read");
  check(!adapter.read(0x0056, 2), "second read refused while busy");
  check(adapter.isBusy(), "engine STILL busy after refusal -> transient/retain");

  std::printf("gwg dispatch-stall discriminator: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
