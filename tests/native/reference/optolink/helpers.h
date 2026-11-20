#pragma once

// The reference engines run on the live engines' clock, so one test script
// drives both sides of the differential target.

#include <cstdlib>

#include "../../../../components/vitohome/optolink_clock.h"

// The copy's GenericInterface aborts on a null port.
#define optolink_abort() abort()

namespace esphome::vitohome::optolink_ref {
using ::esphome::vitohome::optolink::optolink_test_clock_advance;
using ::esphome::vitohome::optolink::optolink_test_clock_freeze;
using ::esphome::vitohome::optolink::optolink_test_clock_release;
}  // namespace esphome::vitohome::optolink_ref
