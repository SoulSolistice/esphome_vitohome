/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#pragma once

#include "../protocol/vs2/packet_vs2.h"
#include "converter.h"

namespace esphome::vitohome::optolink {

class Datapoint {
 public:
  Datapoint(const char* name, uint16_t address, uint8_t length, const Converter& converter);

  explicit operator bool() const;
  const char* name() const;
  uint16_t address() const;
  uint8_t length() const;
  const Converter& converter() const;

  VariantValue decode(const uint8_t* data, uint8_t length) const;
  VariantValue decode(const PacketVS2& packet) const;
  void encode(uint8_t* buf, uint8_t len, const VariantValue& value) const;

 protected:
  const char* _name;
  uint16_t _address;
  uint8_t _length;
  const Converter* _converter;
};

}  // namespace esphome::vitohome::optolink
