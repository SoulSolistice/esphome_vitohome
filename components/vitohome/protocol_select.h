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
inline constexpr const char* PROTOCOL_NAME = "KW (VS1)";
#elif defined(VITOHOME_PROTOCOL_GWG)
using SelectedProtocol = optolink::GWG;
inline constexpr const char* PROTOCOL_NAME = "GWG";
#else
using SelectedProtocol = optolink::P300;
inline constexpr const char* PROTOCOL_NAME = "P300 (VS2)";
#endif

}  // namespace esphome::vitohome
