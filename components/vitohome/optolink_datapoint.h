/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or LICENSE.optolink.

Modified as part of vitohome - see NOTICE.md.
*/

// Include guards, not #pragma once: see optolink_types.h.
#ifndef VITOHOME_OPTOLINK_DATAPOINT_H
#define VITOHOME_OPTOLINK_DATAPOINT_H

#include <cstdint>

namespace esphome::vitohome::optolink {

// Vestigial converter tag: every Datapoint is built with `noconv`, and the
// component decodes and encodes the raw bytes itself (decode.h). The tag stays
// so the Datapoint constructor and the codegen that emits it stay as they
// are.
class Converter {};
class NoconvConvert : public Converter {};
inline NoconvConvert noconv;

// A name, an address and a length; the converter is accepted and not kept.
class Datapoint {
 public:
  Datapoint(const char *name, uint16_t address, uint8_t length, const Converter & /*converter*/)
      : name_(name), address_(address), length_(length) {}

  const char *name() const { return name_; }
  uint16_t address() const { return address_; }
  uint8_t length() const { return length_; }

 private:
  const char *name_;
  uint16_t address_;
  uint8_t length_;
};

}  // namespace esphome::vitohome::optolink

#endif  // VITOHOME_OPTOLINK_DATAPOINT_H
