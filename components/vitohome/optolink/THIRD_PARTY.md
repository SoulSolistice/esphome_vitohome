# Third-party software: Optolink protocol engine

The `optolink/` subtree is a vendored and modified copy of the
**VitoWiFi** library by Bert Melis.

| | |
|---|---|
| Upstream project | https://github.com/bertmelis/VitoWiFi |
| Exact commit vendored | `edc059a7c3df3de0a5de089ebc1bdbfc19ca6faa` |
| Upstream license | MIT (Copyright (c) 2023 Bert Melis) |
| License text | see [`LICENSE.optolink`](./LICENSE.optolink) |

Only the engine sources from the upstream `src/` tree were vendored; the
upstream `test/` tree was not. The engine's regression coverage is the
host-side decode and VS2-transaction tests in `tests/native/` (see
[`tests/native/README.md`](../../../tests/native/README.md)), which run against
this in-tree copy.

This component as a whole is licensed under the GPLv3. Combining MIT-licensed
sources into a GPLv3 work is permitted; the combined work is distributed under
the GPLv3. The per-file MIT notices are retained at the top of every vendored
file, each annotated with a single line noting it was modified as part of
vitohome and pointing back to this file.

This file documents both sides of vitohome's relationship to upstream VitoWiFi:
**what was changed in the vendored copy** (Part 1, the license-relevant
divergences) and **what was deliberately left unchanged and routed around in the
component above** (Part 2, candidate fixes to propose upstream). The deeper
*why* behind both — the decode-in-component architecture and the precision and
type-safety hazards that motivate it — is in
[`docs/design_notes.md`](../../../docs/design_notes.md) §1 and §4.

---

## Part 1 — Modifications applied during vendoring

These are intentional divergences from upstream `edc059a7`:

1. **Namespace & class rename (de-branding).** `namespace VitoWiFi` ->
   `esphome::vitohome::optolink`; `namespace VitoWiFiInternals` ->
   `esphome::vitohome::optolink::internals`. The umbrella template class
   `VitoWiFi::VitoWiFi<PROTOCOLVERSION>` (whose name collided with its
   namespace) is renamed to `OptolinkEngine<PROTOCOLVERSION>`, with the
   concrete per-protocol engines named `VS2Engine` / `VS1Engine` /
   `GWGEngine`. The logging/helper macros were renamed off the old brand
   (`vw_*` -> `optolink_*`). No `VitoWiFi`/`vitowifi` token remains in code,
   paths, includes or build files; the only remaining mentions are this
   attribution prose and the per-file MIT headers.

2. **Platform serial adapters removed.** `HardwareSerialInterface`,
   `SoftwareSerialInterface` and `LinuxSerialInterface` (and the
   platform-gated constructors that used them in `VS2`/`VS1`/`GWG`) were
   deleted. The component only ever constructs an engine through the
   duck-typed `template<class C>` constructor with its own
   `ESPHomeUARTInterface`. `SerialInterface.h` and `GenericInterface.h` are
   kept.

3. **Logging reworked.** The upstream `Logging.h` fell back to a PC branch
   that pulled in `<iostream>`/`<iomanip>` under pure ESP-IDF and wrote
   verbose state transitions to `std::cout` on host. It is replaced with
   no-op-by-default macros that, when `VITOHOME_DEBUG_OPTOLINK` is defined,
   route to the ESP-IDF logger (`esp_log.h`, available under both the
   esp-idf and arduino frameworks via `ESP_PLATFORM`). Host builds are silent
   by default.

4. **Packet buffers modernized.** `PacketVS2`, `PacketVS1` and `PacketGWG`
   replaced their `malloc`/`free`/`realloc` + `_allocatedLength` buffers with
   fixed `std::array`s (VS2: `kMaxFrame=256`, the exact protocol-complete
   bound; VS1/GWG: `kMaxFrame=260`). The engine-level `_responseBuffer`
   `malloc` + `_expandResponseBuffer()` + `_allocatedLength` in `VS1`/`GWG`
   were likewise replaced by fixed `std::array<uint8_t, 256>`. Frames that
   would exceed the fixed bound now fail soft (return `false`) instead of
   growing. With the raw buffer gone, the previously-deleted copy operations
   on the packet classes are restored.

5. **VS2 write-payload guard.** Because a VS2 write stores its P300 length
   byte as `0x05 + len`, `PacketVS2::createPacket` now rejects write payloads
   longer than 250 bytes.

6. **Named timeouts.** The inline magic-number timeouts were lifted to named
   `static constexpr` members, per engine (values byte-identical to
   upstream): VS2 `REQUEST_TIMEOUT_MS=4000`, `HANDSHAKE_RETRY_MS=3000`,
   `KEEPALIVE_INTERVAL_MS=3000`; VS1 `REQUEST_TIMEOUT_MS=4000`,
   `ENQ_RESET_INTERVAL_MS=3000`, `SYNC_WINDOW_MS=50`; GWG
   `REQUEST_TIMEOUT_MS=3000` (deliberately distinct from VS2/VS1).

7. **GWG one-shot bugfix (behavioral divergence).** Upstream
   `GWG::_tryOnResponse()` did not clear `_currentDatapoint` after a
   successful response, unlike `VS1`/`VS2`.
   The vendored `GWG::_tryOnResponse()` now clears `_currentDatapoint` after
   invoking the callback, matching VS1/VS2. This is the only intentional
   change to on-wire/runtime behavior; everything else preserves upstream
   protocol behavior.

---

## Part 2 — Upstream limitations left in place (worked around)

These are upstream shortcomings the vendored copy does **not** change. vitohome
routes around each one in its own layer rather than in the engine, so **none of
them block the component** — they are written up here as candidate improvements
to propose upstream, so the workarounds could eventually be retired. The line
/behaviour claims reference `edc059a7`.

### A. Converter arithmetic is `float` — loses bits on 4-byte counters

- **Limitation.** Converter math is done in `float`; a 32-bit counter beyond
  `2**24` loses integer precision before scaling. (The burner-hours case and the
  worked example are in `docs/design_notes.md` §1b.)
- **Upstream proposal.** Read the raw integer into a 64-bit integer and scale in
  `double`, narrowing only at the final cast. For the existing `float`-returning
  API this is a drop-in internal change; an additional `double`-returning
  accessor would let callers avoid the narrowing entirely.
- **vitohome status.** Worked around: `decode.h::decode_scaled` reads with
  `read_le()` into `uint64_t`, scales in `double`, narrows last.

### B. `VariantValue` is a tagless union — wrong member reads silent garbage

- **Limitation.** `VariantValue` (still present in the vendored
  `datapoint/converter.h`) records nothing about which member was written, so
  reading the wrong member returns whatever bit pattern is there rather than an
  error, and choosing the right member requires the converter that produced it.
  (Rationale and the canonical bug instance: `docs/design_notes.md` §1a.)
- **Upstream proposal.** Tag the union (store which member is active) and have
  accessors check the tag, or return `std::optional<T>` / an explicit type enum
  alongside the value, so "read as the wrong type" becomes a detectable error.
- **vitohome status.** Worked around by never using the converters: every
  `Datapoint` is `noconv` and the component decodes the raw payload itself.

### C. Missing built-in converters for common Vitosoft conversions

- **Limitation.** The Vitosoft data uses conversions VitoWiFi doesn't ship
  presets for (e.g. `Sec2Minute`, and scaled forms beyond the common ones).
- **Upstream proposal.** Add the missing scaled converters and document the
  signedness of each, since `Div2`/`Div10` are signed while the `MultN` forms
  are unsigned.
- **vitohome status.** Not needed upstream: converters are modelled in the
  Python layer as `(scale, signed, lengths, encodable)` presets (see
  [`converters.md`](../../../converters.md)) and applied in `decode.h`.
  Conversions that can't be represented as a scale (floats, `DateTimeBCD`, …)
  are surfaced by the catalog generator as commented hints rather than decoded
  wrongly.

### D. Length/range guards are `assert`-based — compiled out under `NDEBUG`

- **Limitation.** The converter guards are `assert`-based and compile out under
  `NDEBUG`, so a release build silently accepts an out-of-range raw value.
  (Why this matters, and the `noconv` assert that is commented out even in
  debug: `docs/design_notes.md` §4.)
- **Upstream proposal.** Promote the load-bearing guards from `assert` to real
  runtime checks that return an error, independent of `NDEBUG`.
- **vitohome status.** Worked around: the encodable-range check runs at
  `esphome config` time in `number.py` and mirrors `decode.h::encode_scaled`
  exactly, and the C++ `encode_scaled` itself range-checks unconditionally
  before transmitting.

### E. No enum/mapped-value converter — enumerated writes have no upstream path

- **Limitation.** VitoWiFi's value model is four numeric converters (`div10`,
  `div2`, `div3600`, `noconv`); each implements both `decode` *and* `encode`, so
  numeric values can be written back through the converter API. There is,
  however, no enum/mapped-value converter — an enumerated control (operating
  mode, party/economy mode) is not a concept VitoWiFi's converters represent, so
  its value cannot be encoded or written through that API at all. This is the
  write-side counterpart of the missing-scaled-converter gap in C and the
  never-use-the-converters decision in B.
- **Upstream proposal.** Either add a mapped/lookup converter for enumerated
  datapoints, or document that enumerated values are the caller's
  responsibility; the numeric `encode` path already works and needs nothing.
- **vitohome status.** Worked around uniformly: every `Datapoint` is `noconv`
  and **all** writes — numeric (`number`) and enumerated (`select`) — go through
  the engine's raw-bytes `write(const Datapoint&, const uint8_t*, uint8_t)`
  overload, with the component doing the encode itself in `decode.h`
  (`encode_scaled` for numbers; the selected option's little-endian raw value for
  selects). This is what lets a `select` be written at all despite the absence of
  an enum converter, and it is the same path that carries the command/state
  **two-address split** for mode controls — a Viessmann *device* behaviour (some
  datapoints accept a write at one register but expose the resulting state at
  another), not a VitoWiFi limitation, documented in
  [`converters.md`](../../../converters.md) and `docs/design_notes.md` §5.
