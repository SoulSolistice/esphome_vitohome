/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#include "constants.h"

namespace esphome::vitohome::optolink {

const char* errorToString(OptolinkResult error) {
  if (error == OptolinkResult::TIMEOUT) {
    return "timeout";
  } else if (error == OptolinkResult::LENGTH) {
    return "length";
  } else if (error == OptolinkResult::NACK) {
    return "nack";
  } else if (error == OptolinkResult::CRC) {
    return "crc";
  } else if (error == OptolinkResult::ERROR) {
    return "error";
  }
  return "invalid error";
}

}  // namespace esphome::vitohome::optolink
