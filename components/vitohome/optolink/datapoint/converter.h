/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#pragma once

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "../logging.h"
#include "conversion_helpers.h"

namespace esphome::vitohome::optolink {

class VariantValue {
 public:
  explicit VariantValue(uint8_t value) : _value(value) {}
  explicit VariantValue(uint16_t value) : _value(value) {}
  explicit VariantValue(uint32_t value) : _value(value) {}
  explicit VariantValue(uint64_t value) : _value(value) {}
  explicit VariantValue(float value) : _value(value) {}
  explicit VariantValue(bool value) : _value(value) {}
  operator uint8_t() const { return _value._uint8Val; }
  operator uint16_t() const { return _value._uint16Val; }
  operator uint32_t() const { return _value._uint32Val; }
  operator uint64_t() const { return _value._uint64Val; }
  operator float() const { return _value._floatVal; }
  operator bool() const { return _value._uint8Val; }

 protected:
  union _Value {
    _Value(uint8_t v) : _uint8Val(v) {}
    _Value(uint16_t v) : _uint16Val(v) {}
    _Value(uint32_t v) : _uint32Val(v) {}
    _Value(uint64_t v) : _uint64Val(v) {}
    _Value(float v) : _floatVal(v) {}
    _Value(bool v) : _uint8Val(v) {}
    uint8_t _uint8Val;
    uint16_t _uint16Val;
    uint32_t _uint32Val;
    uint64_t _uint64Val;
    float _floatVal;
  } _value;
};

class Converter {
 public:
  virtual VariantValue decode(const uint8_t* data, uint8_t len) const = 0;
  virtual void encode(uint8_t* buf, uint8_t len, const VariantValue& val) const = 0;
  bool operator==(const Converter& rhs) const { return (this == &rhs); }
};

// The only converter the component ever instantiates. Every Datapoint is built
// with `noconv`; the component decodes/encodes raw payloads itself in
// components/vitohome/decode.h (see docs/design_notes.md SS1). The upstream
// scaling converters (Div10/Div2/Div3600) were unused dead code and were
// removed during vendoring -- their scaling now lives, host-tested and in
// double precision, in decode.h.
class NoconvConvert : public Converter {
 public:
  VariantValue decode(const uint8_t* data, uint8_t len) const override;
  void encode(uint8_t* buf, uint8_t len, const VariantValue& val) const override;
};

extern NoconvConvert noconv;

}  // namespace esphome::vitohome::optolink
