/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.

Umbrella header for the in-tree Viessmann Optolink protocol engine.

Upstream exposed `template<class PROTOCOLVERSION> class VitoWiFi` inside a
`namespace VitoWiFi`, so the class name collided with its namespace and had
to be fully qualified everywhere. Here the namespace is
`esphome::vitohome::optolink` and the class is renamed `OptolinkEngine`, so
the collision is gone. Protocol selection is done with the empty tag types
P300 / KW / GWG and a small trait that maps each tag to its engine class.
*/

#pragma once

#include "constants.h"
#include "datapoint/converter.h"
#include "datapoint/datapoint.h"
#include "protocol/gwg/gwg.h"

namespace esphome::vitohome::optolink {

// Protocol tags: P300 is the VS2 wire protocol, KW the VS1 one.
struct P300 {};
struct KW {};
struct GWG {};

namespace internals {
template<class PROTOCOLVERSION> struct ProtocolEngine;
template<> struct ProtocolEngine<GWG> {
  using type = optolink::GWGEngine;
};
}  // namespace internals

// The GWG engine by protocol tag; the constructor takes the port pointer
// (optolink_port.h). KW and P300 run on optolink_kw.h and optolink_p300.h,
// and the hub on protocol_select.h's SelectedEngine.
template<class PROTOCOLVERSION> using OptolinkEngine = typename internals::ProtocolEngine<PROTOCOLVERSION>::type;

}  // namespace esphome::vitohome::optolink
