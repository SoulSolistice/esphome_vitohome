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
#include "protocol/gwg/packet_gwg.h"
#include "protocol/vs1/packet_vs1.h"
#include "protocol/vs1/vs1.h"
#include "protocol/vs2/packet_vs2.h"
#include "protocol/vs2/parser_vs2.h"
#include "protocol/vs2/vs2.h"

namespace esphome::vitohome::optolink {

// Protocol selector tag types. These supersede the VS2/VS1/GWG tag names
// introduced in 111805f: P300 and KW are the protocol's own domain names
// (P300 == the VS2 wire protocol, KW == the VS1 wire protocol), and using
// them frees the VS2/VS1 identifiers for the engine classes. GWG keeps its
// spelling as a tag but is now distinct from the GWGEngine class.
struct P300 {};
struct KW {};
struct GWG {};

namespace internals {
template<class PROTOCOLVERSION> struct ProtocolEngine;
template<> struct ProtocolEngine<P300> {
  using type = optolink::VS2Engine;
};
template<> struct ProtocolEngine<KW> {
  using type = optolink::VS1Engine;
};
template<> struct ProtocolEngine<GWG> {
  using type = optolink::GWGEngine;
};
}  // namespace internals

// OptolinkEngine<P300> is a drop-in for the former VitoWiFi::VitoWiFi<VS2>.
// The protocol tag selects the concrete engine; the constructor takes the
// duck-typed interface pointer (wrapped internally in GenericInterface<C>).
template<class PROTOCOLVERSION> using OptolinkEngine = typename internals::ProtocolEngine<PROTOCOLVERSION>::type;

}  // namespace esphome::vitohome::optolink
