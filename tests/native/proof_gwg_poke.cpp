// Verifies the GWG sync-poke switch: with SEND_ENQ_POKE off (default) the
// engine must NOT emit an EOT (0x04) while waiting for ENQ; with it on, the
// engine must emit the EOT poke after the interval. Built twice (default
// header, then a copy with the switch flipped) by the surrounding script.
#include <chrono>
#include <cstdio>
#include <thread>

#include "fake_optolink.h"
#include "protocol_adapter.h"

using namespace esphome::vitohome;

int main() {
  FakeOptolink uart;
  ProtocolAdapter adapter(&uart);  // GWGEngine under -DVITOHOME_PROTOCOL_GWG
  adapter.on_response([](const ResponseView&, const optolink::Datapoint&) {});
  adapter.on_error([](optolink::OptolinkResult, const optolink::Datapoint&) {});
  adapter.begin();

  optolink::Datapoint dp("probe", 0x0000, 2, optolink::noconv);
  adapter.read(dp);

  // Pump the loop across a >2ms window with no ENQ ever fed by the fake device.
  for (int i = 0; i < 50; ++i) {
    adapter.loop();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  bool eot = false;
  for (uint8_t b : uart.written()) {
    if (b == 0x04) eot = true;
  }
  std::printf("protocol=%s eot_poke_emitted=%d bytes_written=%zu\n", ProtocolAdapter::protocol_name(), eot ? 1 : 0,
              uart.written().size());
  return 0;
}
