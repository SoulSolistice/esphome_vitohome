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
  // converter.h). It is accepted but neither stored nor read -- the constructor
  // signature and the Python codegen that emits it stay stable (and diverge
  // minimally from upstream), while the component decodes/encodes raw payloads
  // itself in decode.h.
  Datapoint(const char *name, uint16_t address, uint8_t length, const Converter &converter);

  const char *name() const;
  uint16_t address() const;
  uint8_t length() const;

 protected:
  const char *_name;
  uint16_t _address;
  uint8_t _length;
};

}  // namespace esphome::vitohome::optolink
