// Host compile + run proof that OptolinkEngine<SelectedProtocol> builds and
// links for all three vendored engines (P300 / KW / GWG), selected at compile
// time via -DVITOHOME_PROTOCOL_* (protocol_select.h). All three engines share
// one byte-mover API -- read/write on (address, length) primitives and
// callbacks delivering (data, length, address) -- so this proves the uniform
// seam the hub drives directly (there is no adapter layer). It does NOT
// exercise real wire behaviour: the fake UART is silent, so no response
// arrives and no response callback fires, which is exactly the signal the
// hub's start-up protocol verification would act on.
//
// This TU is compiled WITHOUT the datapoint/converter translation units,
// which is itself part of the proof: the engine layer has no Datapoint or
// Converter dependency.
#include <cstdio>

#include "fake_optolink.h"
#include "protocol_select.h"

using namespace esphome::vitohome;

int main() {
  FakeOptolink uart;
  optolink::OptolinkEngine<SelectedProtocol> engine(&uart);

  int responses = 0;
  int errors = 0;

  engine.onResponse([&](const uint8_t* data, uint8_t length, uint16_t address) {
    // Touch every parameter so the compiler proves the shape is usable
    // downstream (the hub wraps these in a ResponseView).
    (void)data;
    (void)length;
    (void)address;
    ++responses;
  });
  engine.onError([&](optolink::OptolinkResult err, uint16_t request_address) {
    (void)err;
    (void)request_address;
    ++errors;
  });

  const bool begun = engine.begin();

  engine.read(0x0800, 2);
  for (int i = 0; i < 5; ++i) engine.loop();

  std::printf("protocol=%-10s begin=%d busy=%d responses=%d errors=%d\n", PROTOCOL_NAME, begun ? 1 : 0,
              engine.isBusy() ? 1 : 0, responses, errors);
  return 0;
}
