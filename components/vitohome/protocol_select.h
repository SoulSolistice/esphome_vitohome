#pragma once

#include "optolink/optolink.h"

namespace esphome::vitohome {

// Compile-time protocol selection. The ESPHome codegen (__init__.py) emits
// exactly one VITOHOME_PROTOCOL_* build flag from the `protocol:` option; the
// default (no flag) is P300, the only protocol exercised on hardware.
//
// All three engines share one byte-mover API (read/write on address/length
// primitives, callbacks delivering (data, length, address)), so the hub drives
// OptolinkEngine<SelectedProtocol> directly -- there is no adapter layer. This
// header is the single place the VITOHOME_PROTOCOL_* flags are interpreted.
#if defined(VITOHOME_PROTOCOL_KW)
using SelectedProtocol = optolink::KW;
inline constexpr const char *PROTOCOL_NAME = "KW (VS1)";
// KW and P300 read the burner control (0x6B, Vitosoft's GFA_READ) through
// gfaRead(); GWG has no such telegram. Hardware-confirmed on KW only. Guards
// every call to it.
#define VITOHOME_HAS_GFA
#elif defined(VITOHOME_PROTOCOL_GWG)
using SelectedProtocol = optolink::GWG;
inline constexpr const char *PROTOCOL_NAME = "GWG";
#else
using SelectedProtocol = optolink::P300;
inline constexpr const char *PROTOCOL_NAME = "P300 (VS2)";
// Only P300 has a remote procedure call telegram (function code 0x07);
// VS2Engine::rpc() exists on that engine alone. Guards every call to it.
#define VITOHOME_HAS_RPC
// See the KW branch; over P300 the GFA read is function code 0x6B, unconfirmed
// on hardware (FunctionCode::GFA_READ).
#define VITOHOME_HAS_GFA
#endif

}  // namespace esphome::vitohome
