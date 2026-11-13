/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#include "constants.h"

namespace esphome::vitohome::optolink {

// GWG WRITE frame layout switch -- see the long comment on the declaration in
// constants.h. Default false preserves this project's pre-existing layout;
// nothing changes until something flips it.
bool gwgWriteEotBeforePayload = false;

// GWG BURST frame switch -- see the long comment on the declaration in
// constants.h. Default true (since 2026-10-13, hardware A/B): a burst-sent
// request omits the leading 0x01, which acknowledges an ENQ it does not answer.
bool gwgBurstOmitsEnqAck = true;

// GWG response deadline -- see the comment on the declaration in constants.h.
// Default: GWGEngine::RESPONSE_TIMEOUT_MS.
uint32_t gwgResponseTimeoutMs = 250;

// GWG ENQ-to-send delay -- see the comment on the declaration in constants.h.
// Default 0: send on the ENQ, unchanged behaviour.
uint32_t gwgEnqSendDelayMs = 0;

const char *errorToString(OptolinkResult error) {
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
  } else if (error == OptolinkResult::DEVICE_ERROR) {
    return "device error";
  }
  return "invalid error";
}

}  // namespace esphome::vitohome::optolink
