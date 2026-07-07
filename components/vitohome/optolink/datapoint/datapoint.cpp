/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#include "datapoint.h"

namespace esphome::vitohome::optolink {

Datapoint::Datapoint(const char* name, uint16_t address, uint8_t length, const Converter& converter)
    : _name(name), _address(address), _length(length), _converter(&converter) {
  // empty
}

const char* Datapoint::name() const { return _name; }

uint16_t Datapoint::address() const { return _address; }

uint8_t Datapoint::length() const { return _length; }

}  // namespace esphome::vitohome::optolink
