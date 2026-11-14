// Translation unit for clang-tidy only (scripts/clang_tidy.sh): pulls in the
// hub headers that build without ESPHome, so their inline code is analysed.
// Not built or run by the proof suite.
#include "address_space.h"
#include "decode.h"
#include "poll_schedule.h"
#include "raw_op.h"

int main() { return 0; }
