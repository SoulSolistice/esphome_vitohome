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
// KW and P300 read the burner control (the KW telegram 0x6B, Vitosoft's
// GFA_READ) through gfaRead(); GWG has no such telegram. Guards every call to
// it.
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
// See the KW branch. A P300 controller refuses 0x6B as a function code, so
// VS2Engine::gfaRead() drops the link to KW for the telegram.
#define VITOHOME_HAS_GFA
// A raw read with a whole function-code byte (VS2Engine::functionRead()), the
// scan console's probe for codes the 5-bit field cannot hold.
#define VITOHOME_HAS_FUNCTION_PROBE
#endif

}  // namespace esphome::vitohome
