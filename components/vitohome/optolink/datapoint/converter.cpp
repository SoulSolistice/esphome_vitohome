/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#include "converter.h"

namespace esphome::vitohome::optolink {

VariantValue NoconvConvert::decode(const uint8_t* data, uint8_t len) const {
  // assert(len == 1 || len == 2 || len == 4);
  if (len == 1) {
    uint8_t retVal = data[0];
    return VariantValue(retVal);
  } else if (len == 2) {
    uint16_t retVal = data[1] << 8 | data[0];
    return VariantValue(retVal);
  } else if (len == 4) {
    uint32_t retVal = data[3] << 24 | data[2] << 16 | data[1] << 8 | data[0];
    return VariantValue(retVal);
  } else {
    // decoding should be done in user code
    uint32_t retVal = 0;
    return VariantValue(retVal);
  }
}

void NoconvConvert::encode(uint8_t* buf, uint8_t len, const VariantValue& val) const {
  // assert(len == 1 || len == 2 || len == 4);
  if (len == 1) {
    uint8_t srcVal = val;
    buf[0] = srcVal;
  } else if (len == 2) {
    uint16_t srcVal = val;
    buf[1] = srcVal >> 8;
    buf[0] = srcVal & 0xFF;
  } else if (len == 4) {
    uint32_t srcVal = val;
    buf[3] = srcVal >> 24;
    buf[2] = srcVal >> 16;
    buf[1] = srcVal >> 8;
    buf[0] = srcVal & 0xFF;
  } else {
    // encoding should be done by user
    std::memset(buf, 0, len);
  }
}

NoconvConvert noconv;

}  // namespace esphome::vitohome::optolink
