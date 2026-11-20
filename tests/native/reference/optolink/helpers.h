#pragma once

// The reference engines run on the live engines' clock, so one test script
// drives both sides of the differential target.

#include "../../../../components/vitohome/optolink/helpers.h"

namespace esphome::vitohome::optolink_ref {
using ::esphome::vitohome::optolink::optolink_test_clock_advance;
using ::esphome::vitohome::optolink::optolink_test_clock_freeze;
using ::esphome::vitohome::optolink::optolink_test_clock_release;
}  // namespace esphome::vitohome::optolink_ref
