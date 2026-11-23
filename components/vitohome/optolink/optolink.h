/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.

Umbrella header for what is left of the in-tree Optolink library: the
constants, the datapoint and its converters, and the protocol tags. The
engines are the component's own optolink_kw.h, optolink_p300.h and
optolink_gwg.h, selected in protocol_select.h.
*/

#pragma once

#include "constants.h"
#include "datapoint/converter.h"
#include "datapoint/datapoint.h"

namespace esphome::vitohome::optolink {

// Protocol tags: P300 is the VS2 wire protocol, KW the VS1 one.
struct P300 {};
struct KW {};
struct GWG {};

}  // namespace esphome::vitohome::optolink
