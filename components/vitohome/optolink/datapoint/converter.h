/*
Copyright (c) 2023 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.
For a copy, see <https://opensource.org/licenses/MIT> or the LICENSE file.

Modified as part of vitohome (vendored & de-branded) - see THIRD_PARTY.md.
*/

#pragma once

namespace esphome::vitohome::optolink {

// Vestigial converter tag. Upstream's converter layer -- the virtual
// decode/encode pair returning the tagless `VariantValue` union -- is fully
// removed from the vendored copy (THIRD_PARTY.md items 13/15): every
// Datapoint is built with `noconv` and the component decodes/encodes raw
// payloads itself, host-tested and in double precision, in
// components/vitohome/decode.h (see docs/design_notes.md SS1). The empty tag
// and the `noconv` global remain only so the Datapoint constructor signature
// (and the Python codegen that emits it) stays stable.
class Converter {};

class NoconvConvert : public Converter {};

extern NoconvConvert noconv;

}  // namespace esphome::vitohome::optolink
