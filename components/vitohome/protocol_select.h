#pragma once

#include "optolink/optolink.h"
#include "optolink_gwg.h"
#include "optolink_kw.h"
#include "optolink_p300.h"

namespace esphome::vitohome {

// Compile-time protocol selection. The ESPHome codegen (__init__.py) emits
// exactly one VITOHOME_PROTOCOL_* build flag from the `protocol:` option; the
// default (no flag) is P300. This header is the single place the flags are
// interpreted.
//
// SelectedEngine<Port> is the selected protocol's exchange: submit() a
// Request, one Result per request (optolink_exchange.h).
#if defined(VITOHOME_PROTOCOL_KW)
using SelectedProtocol = optolink::KW;
template<class Port> using SelectedEngine = optolink::KwExchange<Port>;
inline constexpr const char *PROTOCOL_NAME = "KW (VS1)";
// KW and P300 read the burner control (the KW telegram 0x6B, Vitosoft's
// GFA_READ); GWG has no such telegram. Guards every use of it.
#define VITOHOME_HAS_GFA
#elif defined(VITOHOME_PROTOCOL_GWG)
using SelectedProtocol = optolink::GWG;
template<class Port> using SelectedEngine = optolink::GwgExchange<Port>;
inline constexpr const char *PROTOCOL_NAME = "GWG";
#else
using SelectedProtocol = optolink::P300;
template<class Port> using SelectedEngine = optolink::P300Exchange<Port>;
inline constexpr const char *PROTOCOL_NAME = "P300 (VS2)";
// Only P300 has a remote procedure call telegram (function code 0x07).
// Guards every use of it.
#define VITOHOME_HAS_RPC
// See the KW branch. A P300 controller refuses 0x6B as a function code, so
// the exchange detours to KW for the telegram.
#define VITOHOME_HAS_GFA
#endif

}  // namespace esphome::vitohome
