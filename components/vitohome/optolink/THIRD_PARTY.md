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
   invoking the callback, matching VS1/VS2.

8. **GWG response-completion fix (behavioral divergence).** Upstream
   `GWG::_receive()` completed a transaction when the received byte count
   equalled the **request frame length** (`_currentRequest.length()` -- 5 for
   every read, `len + 5` for a write) and reported that frame length as the
   payload length. A GWG read of any length != 5 could therefore never
   complete and always timed out. The vendored engine completes a **read** on
   `_currentDatapoint.length()` received bytes -- source-confirmed against
   vcontrold's GWG protocol definition (`getaddr` = `SEND 01 CB $addr $hexlen
   04; RECV $len`) -- and a **write** on a single ack byte, following the
   KW-family write-ack convention. The write side is **model-derived** for GWG
   itself: vcontrold's GWG `setaddr` entry is a stub (`SYNC;RECV 1`), so no
   independent GWG reference exists -- but the 1-byte-ack convention it
   follows is now **hardware-confirmed on the KW sibling protocol** (see
   item 11). GWG remains unverified on hardware either way. Host-proven by `tests/native/proof_gwg_read.cpp` (read and
   write completion, exact wire frames); the same proof fails 8 checks
   against the upstream behaviour.

9. **VS2 non-RESPONSE frame guard (behavioral divergence).** Upstream
   `VS2::_receive()` delivered **any** complete, checksum-valid frame through
   the response callback -- including a device ERROR frame (PacketType
   `0x03`), whose payload was then decoded and published as data. The
   vendored engine routes a complete frame whose type is not `RESPONSE` to
   the error callback instead, as **`OptolinkResult::DEVICE_ERROR`** -- an
   enum value added to upstream's `OptolinkResult` precisely so callers can
   tell a complete, checksum-valid device answer (proof of a live peer
   speaking this protocol; the hub's link-health tracking counts it as
   alive) apart from `OptolinkResult::ERROR`, which the parser raises for
   malformed traffic (an invalid length/type/function code after a start
   byte -- possibly line noise, proof of nothing). The link-layer
   choreography is unchanged: the frame is still ACKed and the engine
   proceeds to IDLE. Host-proven by `tests/native/proof_vs2_guards.cpp`
   (test A); the golden-master transaction harness (8/8, frames lifted from
   live hardware captures) is unaffected.

10. **VS2 parser reset on engine reset (behavioral divergence).** Upstream
    left the byte-at-a-time `ParserVS2` state untouched on the engine's
    RESET path (`ParserVS2::reset()` existed but had **zero call sites**), so
    a request that timed out mid-frame left the parser stuck mid-PAYLOAD and
    the next transaction's frame was consumed as payload continuation --
    one extra failed transaction (CS_ERROR) after every mid-frame timeout
    before self-healing. The vendored `_reset()` now also resets the parser,
    matching the RX-buffer drain it already performed. Host-proven by
    `tests/native/proof_vs2_guards.cpp` (test B: the first post-recovery
    transaction succeeds).

11. **VS1 write-ack completion fix (behavioral divergence,
    hardware-confirmed).** Upstream `VS1::_receive()` completed a WRITE when
    `_currentDatapoint.length()` response bytes had arrived -- but the device
    acks a KW write (`0xF4`) with a **single `0x00` byte**. Live capture from
    a VScotHO1_72 (`0x20CB`), 2026-07-02: the 8-byte clock write to `0x088E`
    received its `0x00` ack ~125 ms after the frame, upstream's check then
    waited for 8 bytes and reported a timeout ~4 s later -- although the
    device had applied the write. The coincidence `len == 1` for the common
    1-byte writes (Betriebsart, setpoints) is what masked this. The vendored
    engine completes a write on the single ack byte and logs a warning if it
    is not the documented `0x00` (vcontrold's KW `setaddr` -- `RECV 1 SR` --
    also reads exactly one byte and does not validate its value). Host-proven
    by `tests/native/proof_vs1_write.cpp`, whose vector mirrors the capture
    byte-for-byte; it fails 7 checks against the upstream behaviour.

12. **VS2 parser zero-payload out-of-bounds write fix (behavioral divergence,
    host-proven).** Upstream `ParserVS2::parse()` enters the `PAYLOAD` step for
    any payload-bearing frame type whose payload-length byte passes the
    `b != length()-6` guard -- including a **zero-length** payload, where
    `length()-6 == 0 == b` passes. With `_payloadLength == 0`, the first payload
    byte post-decrements it to `255`, so the `== 0` completion check never fires
    and every subsequent byte writes `_packet[6 + dataLength() - _payloadLength]`
    at a wildly negative index: an out-of-bounds write through
    `std::array::operator[]` (upstream: a `malloc` buffer), reachable from
    garbled RX **before the checksum is verified**. Inherited byte-identical from
    `edc059a`; the `std::array` buffer modernization (item 4) changed the failure
    mode but not the arithmetic. The vendored parser routes a zero-length payload
    straight to `CHECKSUM`. Host-proven by `tests/native/proof_vs2_zero_payload.cpp`
    under AddressSanitizer/UBSan: against the pre-fix parser the valid-frame
    scenario never completes (the checksum byte is consumed as phantom payload)
    and ASan traps the out-of-bounds write on the stray-byte scenario; the fix
    completes cleanly with `dataLength() == 0` and rejects stray bytes as a
    checksum error.

13. **Dead scaling converters removed (structural, no behavior change).**
    `Div10Convert` / `Div2Convert` / `Div3600Convert` and their `div10` / `div2`
    / `div3600` globals were unreferenced dead code -- every `Datapoint` uses
    `noconv`, and all scaling is done host-tested and in `double` by
    `decode.h`. They are removed from `datapoint/converter.{h,cpp}`; only
    `NoconvConvert` / `noconv` remains. This deletes the last runtime `malloc`'s
    only reachable-in-principle sibling and shrinks the tagless-`VariantValue`
    surface (see Part 2 A/B). No on-wire or runtime behavior changes.

14. **Engine API reshaped to a byte-mover (structural, no behavior change).**
    The three engines (`VS2Engine` / `VS1Engine` / `GWGEngine`) no longer take a
    `Datapoint`. `read(uint16_t address, uint8_t length)` and
    `write(uint16_t address, const uint8_t* data, uint8_t length)` take
    primitives, and the response/error callbacks deliver
    `(const uint8_t* data, uint8_t length, uint16_t address)` /
    `(OptolinkResult, uint16_t address)` -- so the engine headers no longer
    include `datapoint.h`, and the engine knows nothing about datapoints,
    converters or scaling. The dead `write(const Datapoint&, const VariantValue&)`
    overload (one per engine -- its `malloc` was each engine's only runtime
    allocation) is deleted. The single-in-flight guard
    moves from a `_currentDatapoint` sentinel to a `_busy` flag plus a retained
    `_currentAddress`; correlation of a response to its request stays the
    caller's job (the hub already tracks its own in-flight context). P300 still
    surfaces the address echoed in the response frame; KW/GWG echo the retained
    request address. The component-level `ProtocolAdapter` first collapsed its
    former per-protocol `#if` response branch into one uniform path, and was
    then removed entirely: the hub drives `OptolinkEngine<SelectedProtocol>`
    directly (`protocol_select.h` holds the compile-time selection). Behavior on the wire is
    unchanged -- proven by the existing transaction/guard/completion harnesses,
    which pass against the reshaped engines.

15. **Residual dead-code sweep (structural, no behavior change).** The last
    unreferenced upstream surfaces are removed: `conversion_helpers.{h,cpp}`
    (the `encodeSchedule`/`decodeSchedule` codec -- superseded by `decode.h`'s
    Schaltzeiten codec; the dead `encodeSchedule` also carried an upstream
    logic bug, `if (hour <= 23 || minutes <= 59)` is always true and should
    have been `&&`); `Datapoint::decode()` (both overloads),
    `Datapoint::encode()`, `Datapoint::operator bool()` and the `converter()`
    accessor (all zero call sites -- decode/encode happen in the component's
    `decode.h`); the tagless `VariantValue` union and the `Converter`
    decode/encode virtuals (see Part 2 B -- the hazard class is now deleted,
    not merely avoided; `Converter`/`NoconvConvert`/`noconv` remain only as an
    empty vestigial tag keeping the `Datapoint` constructor signature and the
    Python codegen stable); `getState()` on all three engines; and the
    unreferenced `START_PAYLOAD_LENGTH` constant with its configuration macro.
    `ParserVS2` now checks the frame's function code against the
    `FunctionCode::READ/WRITE/RPC` constants instead of bare literals, which
    also puts the previously-unreferenced `RPC` constant to use.
    A follow-up pass removed the remaining zero-call-site packet accessors:
    `operator bool()` on all three packet classes; `address()`,
    `dataLength()` and `data()` on `PacketVS1` and `PacketGWG` (their engines
    complete out of `_responseBuffer`, never the request packet); and `id()`
    on `PacketVS2`. `PacketVS2` keeps `address()`/`dataLength()`/`data()`,
    which feed the response callback and the parser. The packet headers also
    dropped their vestigial `<cassert>` and `helpers.h` includes (no assert
    and no helper macro is used in any packet translation unit).

16. **VS2 RESPONSE packet payload guards (latent-bug fix, no behavior change
    from any current call site).** `PacketVS2::createPacket`'s payload copy
    runs for `fc == WRITE || pt == RESPONSE` (a read response echoes data
    back), but upstream keyed the null-data check, the `len > 250` cap and
    the buffer-size computation on `WRITE` alone. A RESPONSE with
    `len` 251-255 therefore passed a size check computed for 6 bytes and then
    copied `len` bytes -- an out-of-bounds write past the 256-byte packet
    array -- and a RESPONSE with a null `data` pointer was dereferenced. Both
    were latent: the engines only ever build REQUEST packets. The vendored
    copy keys every guard on one `has_payload` condition matching the copy
    loop, and serialises a payload-bearing RESPONSE with the
    protocol-correct length byte `0x05 + len` (previously `0x05` regardless
    -- an on-wire difference confined to the same never-exercised path).
    Host-proven by `tests/native/proof_packet_vs2_response.cpp` under
    ASan/UBSan; the same proof traps with a stack-buffer-overflow against the
    upstream-shaped code.

Items 7-12 are the only intentional changes to on-wire/runtime behavior;
items 13-16 are structural (no behavior change from any call site that
exists -- item 16 alters bytes only on a RESPONSE-construction path nothing
exercises). Everything else preserves upstream protocol behavior. Each
behavioral item is covered by a host proof that fails against the upstream
code, so none of them rests on inspection alone.

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

- **Limitation.** Upstream's `VariantValue` (the tagless union its converter
  `decode()`/`encode()` returned) records nothing about which member was
  written, so reading the wrong member returns whatever bit pattern is there
  rather than an error, and choosing the right member requires the converter
  that produced it. (Rationale and the canonical bug instance:
  `docs/design_notes.md` §1a.)
- **Upstream proposal.** Tag the union (store which member is active) and have
  accessors check the tag, or return `std::optional<T>` / an explicit type enum
  alongside the value, so "read as the wrong type" becomes a detectable error.
- **vitohome status.** Resolved by removal: every `Datapoint` is `noconv` and
  the component decodes the raw payload itself, and the vendored copy has now
  deleted `VariantValue` and the converter decode/encode virtuals outright
  (Part 1, items 13/15). `Converter`/`NoconvConvert` survive only as an empty
  vestigial tag; the proposal above remains relevant to upstream.

### C. Missing built-in converters for common Vitosoft conversions

- **Limitation.** The Vitosoft data uses conversions VitoWiFi doesn't ship
  presets for (e.g. `Sec2Minute`, and scaled forms beyond the common ones).
- **Upstream proposal.** Add the missing scaled converters and document the
  signedness of each, since `Div2`/`Div10` are signed while the `MultN` forms
  are unsigned.
- **vitohome status.** Not needed upstream: converters are modelled in the
  Python layer as `(scale, signed, lengths, encodable)` presets (see
  [`converters.md`](../../../docs/converters.md)) and applied in `decode.h`.
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
  the engine's raw byte-mover `write(uint16_t address, const uint8_t *data,
  uint8_t length)` (the reshaped API of Part 1, item 14), with the component
  doing the encode itself in `decode.h`
  (`encode_scaled` for numbers; the selected option's little-endian raw value for
  selects). This is what lets a `select` be written at all despite the absence of
  an enum converter, and it is the same path that carries the command/state
  **two-address split** for mode controls — a Viessmann *device* behaviour (some
  datapoints accept a write at one register but expose the resulting state at
  another), not a VitoWiFi limitation, documented in
  [`converters.md`](../../../docs/converters.md) and `docs/design_notes.md` §5.
