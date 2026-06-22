/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace vitohome {
namespace optolink {
namespace internals {

class SerialInterface {
 public:
  virtual ~SerialInterface() {}
  virtual bool begin() = 0;
  virtual void end() = 0;
  virtual std::size_t write(const uint8_t *data, uint8_t length) = 0;
  virtual uint8_t read() = 0;
  virtual size_t available() = 0;
};

}  // namespace internals
}  // namespace optolink
}  // namespace vitohome
}  // namespace esphome
