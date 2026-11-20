// Host compile + run proof that the hub's engine, SelectedEngine<Port> in
// protocol_select.h, builds and links for each protocol (-DVITOHOME_PROTOCOL_*)
// and takes the request read_request() builds. It does not exercise the wire:
// the fake UART is silent, so no answer arrives, which is what the hub's
// start-up verification acts on.
//
// Compiled without the datapoint and converter translation units: the engine
// layer depends on neither.
#include <cstdio>

#include "address_space.h"
#include "fake_optolink.h"
#include "protocol_select.h"

using namespace esphome::vitohome;

int main() {
  FakeOptolink uart;
  SelectedEngine<FakeOptolink> engine(&uart);

  int responses = 0;
  int errors = 0;
  engine.on_result([&](const optolink::Result &result) {
    if (result.status == optolink::OptolinkResult::OK) {
      ++responses;
    } else {
      ++errors;
    }
  });

  const bool begun = engine.begin();

  optolink::Request request;
  const bool taken = read_request(AddressSpace::OWN, 0x00F8, 2, nullptr, 0, request) && engine.submit(request);
  for (int i = 0; i < 5; ++i)
    engine.loop();

  std::printf("protocol=%-10s begin=%d taken=%d busy=%d responses=%d errors=%d\n", PROTOCOL_NAME, begun ? 1 : 0,
              taken ? 1 : 0, engine.busy() ? 1 : 0, responses, errors);
  return begun && taken ? 0 : 1;
}
