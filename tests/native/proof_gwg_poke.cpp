// The GWG link sends no EOT (0x04) while it waits for the device's ENQ: it
// has no sync poke, unlike vcontrold's GWG sync. The surrounding script checks
// the printed eot_poke_emitted=0.
#include <cstdio>

#include "fake_optolink.h"
#include "gwg_calls.h"
#include "protocol_select.h"

using namespace esphome::vitohome;

int main() {
  // Before the engine is constructed, so every clock sample comes from the
  // same frozen clock.
  optolink::optolink_test_clock_freeze();

  FakeOptolink uart;
  gwg_calls::Engine<FakeOptolink> adapter(&uart);
  adapter.onResponse([](const uint8_t *, uint8_t, uint16_t) {});
  adapter.onError([](optolink::OptolinkResult, uint16_t) {});
  adapter.begin();

  adapter.read(0x0000, 2);

  // The watchdog's whole span with no ENQ ever fed by the fake device.
  for (int i = 0; i < 3100; ++i) {
    adapter.loop();
    optolink::optolink_test_clock_advance(1);
  }

  bool eot = false;
  for (uint8_t b : uart.written()) {
    if (b == 0x04)
      eot = true;
  }
  std::printf("protocol=GWG eot_poke_emitted=%d bytes_written=%zu\n", eot ? 1 : 0, uart.written().size());
  return 0;
}
