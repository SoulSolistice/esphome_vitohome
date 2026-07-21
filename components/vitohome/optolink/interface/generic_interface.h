/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#pragma once

#include <cstdlib>  // abort()

#include "serial_interface.h"

namespace esphome::vitohome::optolink {
namespace internals {

template<class C> class GenericInterface : public SerialInterface {
 public:
  explicit GenericInterface(C *interface) : _interface(interface) {
    if (!interface) {
      abort();
    }
  }
  bool begin() override { return _interface->begin(); }
  void end() override { _interface->end(); }
  std::size_t write(const uint8_t *data, uint8_t length) override { return _interface->write(data, length); }
  uint8_t read() override { return _interface->read(); }
  size_t available() override { return _interface->available(); }

 private:
  C *_interface;
};

}  // namespace internals
}  // namespace esphome::vitohome::optolink
