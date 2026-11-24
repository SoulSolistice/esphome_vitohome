// Translation unit for clang-tidy only (scripts/clang_tidy.sh): pulls in the
// hub headers that build without ESPHome, so their inline code is analysed,
// and instantiates the KW, P300 and GWG stacks, whose code is templates.
// Not built or run by the proof suite.
#include "address_space.h"
#include "decode.h"
#include "fake_optolink.h"
#include "optolink_gwg.h"
#include "optolink_kw.h"
#include "optolink_p300.h"
#include "poll_schedule.h"
#include "raw_op.h"
#include "vito_recorder.h"

template class esphome::vitohome::optolink::KwExchange<FakeOptolink>;
template class esphome::vitohome::optolink::P300Exchange<FakeOptolink>;
template class esphome::vitohome::optolink::GwgExchange<FakeOptolink>;

int main() { return 0; }
