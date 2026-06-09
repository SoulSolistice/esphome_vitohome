# Upstreaming to VitoWiFi (notes)

vitohome currently treats VitoWiFi as a pure wire engine and does all value
decode/encode itself (see `docs/stage2_design.md`). That means **none of the
items below block vitohome** — we already work around them. They are written up
here as candidate improvements to propose upstream, so the workarounds could
eventually be retired.

Reference commit for all line/behaviour claims: `edc059a7`.

## 1. Decode in double precision (or templated width)

**Problem.** Converter arithmetic is done in `float`. A 32-bit counter beyond
`2**24` loses integer precision before scaling (the burner-hours case:
212,197,680 s — see the precision analysis in `stage2_design.md`).

**Proposal.** Read the raw integer into a 64-bit integer and scale in `double`,
narrowing only at the final cast. For the existing `float`-returning API this
is a drop-in internal change; an additional `double`-returning accessor would
let callers avoid the narrowing entirely.

**vitohome status.** Worked around: `decode.h::decode_scaled` reads with
`read_le()` into `uint64_t`, scales in `double`, narrows last.

## 2. A discriminated value type

**Problem.** `VariantValue` is a tagless union. Reading the wrong member
returns a silent garbage value rather than an error, and choosing the right
member requires the converter that produced it.

**Proposal.** Tag the union (store which member is active) and have accessors
check the tag, or return `std::optional<T>` / an explicit type enum alongside
the value. This makes "read as the wrong type" a detectable error instead of
undefined-ish behaviour.

**vitohome status.** Worked around by never using the converters: every
`Datapoint` is `noconv` and we decode the raw payload ourselves.

## 3. More built-in converters

**Problem.** The Vitosoft data uses conversions VitoWiFi doesn't ship presets
for (e.g. `Sec2Minute`, and the various scaled forms beyond the common ones).

**Proposal.** Add the missing scaled converters (and document the signedness of
each, since `Div2`/`Div10` are signed while the `MultN` forms are unsigned).

**vitohome status.** Not needed upstream for us — converters are modelled in
the Python layer as `(scale, signed, lengths, encodable)` presets and applied
in `decode.h`. We surface conversions we can't represent (e.g. floats,
`DateTimeBCD`) as commented hints in the catalogue generator rather than
decoding them wrongly.

## 4. Keep runtime length/range guards under `NDEBUG`

**Problem.** VitoWiFi's converter guards are `assert`-based and compile out
under `NDEBUG`, so a release build silently accepts an out-of-range raw value.

**Proposal.** Promote the load-bearing guards from `assert` to real runtime
checks that return an error, independent of `NDEBUG`.

**vitohome status.** Worked around: the encodable-range check runs at
`esphome config` time in `number.py` and mirrors `decode.h::encode_scaled`
exactly, and the C++ `encode_scaled` itself range-checks unconditionally before
transmitting.

---

### If/when these land upstream

vitohome could optionally switch specific datapoints back to library
converters, but the in-component path would likely stay the default: it gives a
single host-testable decode/encode path with no hardware in the loop, which is
worth keeping regardless of what the library does internally.
