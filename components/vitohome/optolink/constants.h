/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#pragma once

#include <cstddef>
#include <cstdint>

#include "optolink_codec_gwg.h"  // GWGAccessMode, PacketGWGType, the access-mode tables
#include "optolink_codec_kw.h"   // PacketVS1Type
#include "optolink_types.h"      // MAX_PAYLOAD, FunctionCode, PacketType, OptolinkResult

namespace esphome::vitohome::optolink {

// Bounds of a GWG response deadline set at run time
// (VitoHomeComponent::set_gwg_response_timeout_ms()). The upper one keeps a
// retried burst read inside the hub's request watchdog.
inline constexpr uint32_t GWG_RESPONSE_TIMEOUT_MIN_MS = 50;
inline constexpr uint32_t GWG_RESPONSE_TIMEOUT_MAX_MS = 2000;

// Upper bound of a GWG ENQ-to-send delay set at run time
// (VitoHomeComponent::set_gwg_enq_send_delay_ms()), well under the shortest
// idle-ENQ gap seen (749 ms).
inline constexpr uint32_t GWG_ENQ_SEND_DELAY_MAX_MS = 100;

// Upper bound of a GWG gap after the 0x01 set at run time
// (VitoHomeComponent::set_gwg_enq_ack_gap_ms()); with the delay above it stays
// under the shortest idle-ENQ gap.
inline constexpr uint32_t GWG_ENQ_ACK_GAP_MAX_MS = 100;

const char *errorToString(OptolinkResult error);

}  // namespace esphome::vitohome::optolink

namespace esphome::vitohome::optolink {
namespace internals {

constexpr struct {
  uint8_t PACKETSTART = 0x41;
  uint8_t ACK = 0x06;
  uint8_t ENQ_ACK = 0x01;
  uint8_t NACK = 0x15;
  uint8_t ENQ = 0x05;
  uint8_t EOT = 0x04;
  uint8_t SYNC[3] = {0x16, 0x00, 0x00};
} ProtocolBytes;

}  // namespace internals
}  // namespace esphome::vitohome::optolink
