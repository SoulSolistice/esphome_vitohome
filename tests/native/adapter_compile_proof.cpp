// Host compile + run proof that ProtocolAdapter + ResponseView build and link
// against all three vendored engines (P300 / KW / GWG), selected at compile
// time via -DVITOHOME_PROTOCOL_*. This proves the protocol-agnostic boundary
// compiles for every protocol and that each engine's callback shape funnels
// into one ResponseView path. It does NOT exercise real wire behaviour -- the
// fake UART is silent, so no response arrives and `established` stays false,
// which is exactly the signal the hub would act on.
#include <cstdio>

#include "fake_optolink.h"
#include "protocol_adapter.h"

using namespace esphome::vitohome;

int main() {
  FakeOptolink uart;
  ProtocolAdapter adapter(&uart);

  int responses = 0;
  int errors = 0;

  adapter.on_response([&](const ResponseView &view, const optolink::Datapoint &dp) {
    // Touch every field so the compiler proves the view is usable downstream.
    (void)view.data;
    (void)view.data_length;
    (void)view.address;
    (void)dp.address();
    ++responses;
  });
  adapter.on_error([&](optolink::OptolinkResult err, const optolink::Datapoint &dp) {
    (void)err;
    (void)dp;
    ++errors;
  });

  const bool begun = adapter.begin();

  optolink::Datapoint dp("probe", 0x0800, 2, optolink::noconv);
  adapter.read(dp);
  for (int i = 0; i < 5; ++i) adapter.loop();

  std::printf("protocol=%-10s begin=%d busy=%d established=%d responses=%d errors=%d\n",
              ProtocolAdapter::protocol_name(), begun ? 1 : 0, adapter.is_busy() ? 1 : 0, adapter.established() ? 1 : 0,
              responses, errors);
  return 0;
}
