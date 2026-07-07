/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#pragma once

#include <cstdint>

#include "converter.h"

namespace esphome::vitohome::optolink {

class Datapoint {
 public:
  // The Converter argument is a vestigial tag (always `noconv`; see
  // converter.h) -- stored but never read. Kept so the constructor signature
  // and the Python codegen that emits it stay stable.
  Datapoint(const char* name, uint16_t address, uint8_t length, const Converter& converter);

  const char* name() const;
  uint16_t address() const;
  uint8_t length() const;

 protected:
  const char* _name;
  uint16_t _address;
  uint8_t _length;
  const Converter* _converter;
};

}  // namespace esphome::vitohome::optolink
