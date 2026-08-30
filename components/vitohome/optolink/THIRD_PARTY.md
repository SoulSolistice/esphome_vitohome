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
   `ENQ_RESET_INTERVAL_MS=3000`, `ENQ_ACK_WINDOW_MS=50` (see item 22); GWG
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

17. **Engine fail-soft on interface-allocation failure (behavioral divergence,
    error handling -- no on-wire or normal-path change).** Upstream's engine
    constructors `abort()` (terminate the firmware) when the serial-interface
    allocation fails: `new (std::nothrow) internals::GenericInterface<C>(interface)`
    returns null on OOM, and the constructor logs and aborts. The vendored
    engines fail soft instead -- the constructor leaves `_interface` null (still
    logging the failure), and `begin()` returns false when `_interface` is null.
    The ESPHome wrapper already treats a false `begin()` as fatal for the
    component (`VitoHomeComponent::setup()`: `if (!vito_->begin()) { ESP_LOGE(...);
    mark_failed(); return; }`), so on a transient allocation failure the
    component is disabled cleanly instead of the whole firmware aborting. The
    vendored engine has no ESPHome dependency, so it signals the failure through
    `begin()`'s return value rather than calling `mark_failed()` itself; the
    destructors were already null-safe (`delete nullptr`). The unreachable
    null-`interface`-argument check in `GenericInterface`'s own constructor -- a
    programmer error, since the interface is always the address of a hub member
    -- is deliberately left aborting. This path (allocation failure) is not
    host-triggerable without allocation injection, so it rests on inspection plus
    the wrapper's existing `begin()` handling rather than a host proof.

18. **GWG response deadline + same-loop send (behavioral divergence,
    hardware-motivated).** Two related changes in `GWGEngine`, both driven by a
    2026-08-23 capture from a live GWG unit -- the first hardware evidence the
    project has for this protocol.

    *Response deadline.* Upstream's only watchdog over `RECEIVE` is
    `REQUEST_TIMEOUT_MS` (3000 ms), and it is measured from `read()`/`write()`,
    so it also covers the ENQ wait in `INIT`. A GWG read response carries no
    framing at all -- it is exactly `_currentLength` raw data bytes, no start
    byte, no type, no checksum -- while a KW-family device emits an unsolicited
    ENQ (`0x05`) roughly once a second whenever it is idle. The consequence:
    a request the device ignored was completed by the *next* idle ENQ, and
    `0x05` was delivered to the caller as a successful payload. On the capture,
    9 of 92 reads returned exactly `0x05` after 903-1476 ms (against 0-90 ms for
    the 79 reads the device actually answered), which surfaced in Home Assistant
    as a plausible-looking 2.5 degC on every `div2` temperature, with no warning
    logged. The vendored engine adds `RESPONSE_TIMEOUT_MS` (250 ms) as a
    wire-activity deadline inside `RECEIVE`, armed at the end of `_send()` and
    re-armed on every received byte; expiry raises
    `OptolinkResult::TIMEOUT`. Note that discarding a leading `0x05` would be
    *wrong*: `0x05` is a legal datapoint value, and timing is the only
    discriminator the protocol offers. 250 ms is ~2.7x the slowest observed
    genuine response and ~3x below the shortest observed idle-ENQ gap (749 ms).

    The deadline is evaluated at the *top* of `_receive()`, before it drains the
    interface, and this ordering is load-bearing rather than incidental. The
    drain re-arms `_lastMillis` on every byte it consumes and `_currentMillis`
    is sampled once per `loop()`, so a deadline checked after the drain can
    never fire in an iteration that read anything -- and the ESP32 UART RX FIFO
    holds the idle ENQ across a long host stall, so the byte is already buffered
    and still looks "fresh" when the stall ends. Checked after the drain the
    guard is therefore inert under exactly the long-loop conditions the capture
    blamed; checked first, anything still buffered past the deadline is treated
    as stale (and flushed, so it cannot be mistaken for a fresh sync byte by the
    next `_init()`). The cost is a discarded genuine response when the host
    stalls beyond 250 ms, which is the right trade: the protocol offers no
    arrival timestamps, so a buffered `0x05` and a buffered genuine datapoint of
    value `0x05` cannot be told apart, and a lost read self-heals on the next
    poll while a bogus one does not.

    The deadline covers **both directions**. A write's single ack byte is held
    to it too (it had 3000 ms before this constant existed). That is deliberate:
    the ack is a bare byte with no framing either, and GWG -- unlike VS1, which
    checks for `0x00` -- does not validate its value, so an idle ENQ can pose as
    a write ack exactly as it did as a read payload.

    *Same-loop send.* Upstream's `_init()` sets `State::SEND` and returns, so the
    request frame is written one `loop()` iteration later. The KW-family master
    has to answer the device's ENQ inside a short window, and on the capture the
    unanswered requests clustered at the start of the poll cycle -- exactly where
    ESPHome's 60 s entity publishes and the API state flush lengthen the loop.
    `_init()` now falls through into `_send()` in the same iteration that
    consumed the ENQ. This is a latency reduction, not a protocol change: the
    bytes and the state sequence are identical.

19. **GWG read access modes (behavioral divergence, spec-sourced).** Upstream
    has no concept of an access mode: `PHYSICAL READ` (`0xCB`) is the only TYPE
    byte it ever emits on a read. `GWGEngine::read()` now takes an optional
    `GWGAccessMode` selecting one of the eight read-direction TYPE bytes from
    the openv wiki's `Protokoll-GWG` "Telegramm Typen" table -- `VIRTUAL`
    `0xC7`, `PHYSICAL` `0xCB`, `EEPROM` `0xAE`, `XRAM` `0xC5`, `PORT` `0x6E`,
    `BE` `0x9E`, `KMBUS RAM` `0x33`, `KMBUS EEPROM` `0x43` (see
    `GWGAccessMode` and `gwgReadTypeByte()` in `constants.h`).
    `PacketGWG::createPacket()` accepts that set instead of the single `0xCB`,
    and `PacketGWG::length()` reports the same 5-byte frame shape for all of
    them -- the latter is not cosmetic: without it a non-PHYSICAL read returned
    length 0, `_send()` wrote nothing, and the engine dropped into `RECEIVE`
    with an empty wire.

    Corroboration. The table is not single-sourced. vcontrold
    (`openv/vcontrold`, `xml/{kw,300}/vcontrold.xml`, `<protocol name="GWG">`)
    independently defines the same TYPE bytes as named macros, inside the same
    frame this engine emits -- `SYNC; GET*ADDR $addr $hexlen 04; RECV $len`,
    i.e. `01 <TYPE> <addr> <len> 04` answered by exactly `<len>` raw bytes,
    which is also the completion rule of item 8:

    | mode | TYPE | vcontrold macro | used by a shipped datapoint? |
    |---|---|---|---|
    | `PHYSICAL` | `0xCB` | `GETADDR` | yes (227 commands) |
    | `VIRTUAL` | `0xC7` | `GETVADDR` | yes -- `getDevType`, `0xF8` len 4 |
    | `EEPROM` | `0xAE` | `GETEADDR` | yes -- `getBrennerStunden1`, `0x17` len 2 |
    | `XRAM` | `0xC5` | `GETXADDR` | yes -- `getExtBA`, `0x00` len 1 |
    | `PORT` | `0x6E` | `GETPADDR` | yes -- `getVentilStatus`, `0x01` len 1 |
    | `BE` | `0x9E` | `GETBADDR` | macro only |
    | `KMBUS_EEPROM` | `0x43` | `GETKMADDR` | macro only (see below) |
    | `KMBUS_RAM` | `0x33` | **none** | none — but see vogod below |

    Two caveats fall out of that. `KMBUS_RAM` (`0x33`) has no vcontrold macro
    and no Vitosoft datapoint uses it -- but it is not single-sourced:
    **speters/vogod** declares the whole GWG command-type set independently
    (`pkg/vogo/fsm.go`), `physicalKmbusRAMRead = 0x33` included, and its
    `writeCmds` map omits both KMBUS modes, reaching the same read-only
    conclusion `gwgModeIsWritable()` encodes. So the byte has two sources; what
    it lacks is any datapoint that uses it. And vcontrold's
    `getkmaddr` command invokes an undefined `GETKMDDR` macro (a typo for
    `GETKMADDR`), so its `0x43` read path is dead code there; the macro
    definition is still evidence for the byte, but nobody has been exercising
    it.

    **Fourth source: vogod.** `speters/vogod` lists all eight read bytes and
    all six write bytes as `CommandType` constants, matching this table
    exactly. It does NOT settle the write frame layout: `prepareCmd()` handles
    only the P300 and KW send states and returns
    `"not implemented: %v (GWG protocol?)"` for every GWG type, and the state
    machine has no GWG states at all. Its `str2CmdType()` also never connects
    Vitosoft's `FCRead` names to those constants -- only `Virtual_READ` and
    `Virtual_WRITE` map to anything, everything else becomes `nop`, and
    `commands.go` then refuses the datapoint as "not readable". So vogod has
    the same P300-centric blind spot this project had before item 19: the table
    is present but unwired (`// TODO: find out more CommandType mappings`).

    **Third source: Viessmann's own data.** Vitosoft carries the access mode
    per datapoint, in `ecnEventType.xml`'s `FCRead` field, and its names map 1:1
    onto the same seven modes -- `Physical_READ`, `Virtual_READ`, `EEPROM_READ`,
    `XRAM_READ`, `Port_READ`, `BE_READ`, `KMBUS_EEPROM_READ`. Across all 22
    Vitosoft `GWG_*` device tokens (4143 events) those seven plus blank are the
    only values that occur, apart from 34 `KBUS_VIRTUAL_READ` (0.8%, a genuine
    K-bus tunnel no access mode reaches). `KMBUS_RAM` again has no counterpart:
    **zero** Vitosoft GWG datapoints use it, so all three sources now agree it
    is the odd one out and nothing in this project will ever emit it.

    This is also what makes the modes load-bearing rather than diagnostic.
    Vitosoft shows GWG's address space is genuinely per-mode: in `GWG_VBES_00`,
    address `0x01` is five different datapoints across four modes (OEM ident on
    `kmbus_eeprom`, max boiler temperature on `eeprom`, three outputs on
    `port`). Address alone is not a datapoint identity under GWG.
    `scripts/gen_catalog.py` therefore maps `FCRead` onto `access:` and emits it
    on every GWG entity (`--protocol`, inferred from the Vitosoft device token).

    It further resolves the three addresses this project had logged
    implausible values for, all of which were being read on `physical`: `0x17`
    is `GWG BEMK50Brennerlaufzeit` on `EEPROM_READ` (vcontrold agrees --
    `getBrennerStunden1`/`geteaddr`), `0xF8..0xFB` are the four `GWG
    SystemIdent` fields on `Virtual_READ` (vcontrold agrees --
    `getDevType`/`getvaddr`), and `0x29` is `GWG Vorlaufmaximaltemperatur HKB
    BEM` on `BE_READ`. The identification read is consequently issued on
    `VIRTUAL` under GWG (`VitoHomeComponent::dispatch_next_`), not `PHYSICAL`.

    Scope. The default is `PHYSICAL`, so every existing call site and every
    config that does not set `access:` is byte-identical to before (guarded by
    `tests/native/proof_gwg_access_mode.cpp` and
    `tests/native/proof_gwg_write_access.cpp`).

    **The write direction is now covered too** (2026-08-24), with one mode
    driving both directions -- Vitosoft pairs every writable GWG datapoint as
    `(EEPROM_READ, EEPROM_WRITE)` or `(BE_READ, BE_WRITE)`, never across modes,
    so a separate write mode could only express wrong things. The write TYPE
    bytes are `VIRTUAL 0xC4`, `PHYSICAL 0xC8`, `EEPROM 0xAD`, `XRAM 0xC3`,
    `PORT 0x6D`, `BE 0x9D`; the wiki gives **no** write byte for either KMBUS
    mode, so `GWGEngine::write()` refuses those outright rather than silently
    substituting another mode (which would write to a different register file).
    Cross-checked against dannerph/esphome_vitoconnect, which implements the
    same six bytes.

    Why this matters more than it looks: **nothing on a GWG unit is
    PHYSICAL-writable.** Across all 22 Vitosoft GWG tokens the writable
    datapoints are `EEPROM_WRITE` (1082) and `BE_WRITE` (280), plus 17
    unreachable `KBUS_VIRTUAL_WRITE` -- and *zero* `PHYSICAL_WRITE`. But `0xC8`
    was the only write byte this engine could emit before this change, and
    dannerph's legacy path defaults to it too. That is the most economical
    explanation for why no GWG write has ever been confirmed to work anywhere,
    including openv issue #467.

    That issue should also be re-read in light of item 18. Its capture is:
    sent `04`, received `05` after **1230 ms**, then `20 53 01 2B` after
    **50 ms** -- i.e. genuine answers in ~50 ms against a ~1230 ms idle-ENQ
    cadence, the same two populations as this project's own capture. Its
    reported "writes only ever return `0x05`" is therefore almost certainly *no
    answer at all*, with the next idle ENQ misread as the ack. It is evidence
    that a physical-mode write was ignored, not that GWG writes are impossible.

    vcontrold contributes nothing here: its GWG `setaddr` is not merely a stub
    but a no-op -- the entire body is `SYNC;RECV 1`, which emits the sync `EOT`
    and reads a byte without ever transmitting an address, length, payload or
    TYPE byte -- and the five GWG `set*` datapoints it declares
    (`setTempRaumNorSollM1` `0x53`, `setTempRaumRedSollM1` `0x54`,
    `setPumpeStatusZirku` `0x01`, `setUmschaltventil` `0x01`,
    `setBrennerStunden1` `0x17`) all route into it, so none has ever worked.

    **One thing remains genuinely unknown: where the `0x04` sits in a write
    frame.** This project emits `01 TYPE ADDR LEN <data...> 04` (EOT last);
    dannerph emits `01`, then `TYPE ADDR LEN 04 <data...>` (EOT before the
    payload); vcontrold's KW sibling emits no terminator at all. The wiki calls
    `0x04` the "Telegramm Ende Byte" and shows no write example. Neither GWG
    layout has hardware evidence, so the choice sits behind
    `gwgWriteEotBeforePayload` (default: this project's pre-existing layout)
    until a unit settles it. Reads are byte-identical under both, which is why
    this never surfaced. Both settings build and pass the full proof suite.

    Status. `PHYSICAL` is the only mode with a vitohome hardware capture behind
    it. The other seven are spec-plus-second-implementation rather than
    verified here -- which for the four vcontrold actually polls with is a good
    deal stronger than "transcribed from a wiki", and for `0x33` is weaker.

20. **GWG timing instrument (diagnostic, no protocol change).** An optional
    `GWGEngine::onTiming()` callback fires once per successfully completed read
    (never for writes, never for failures) reporting the `loop()` gap that
    preceded the iteration which consumed the device's ENQ, and the
    frame-written-to-response-received delta. Motivated by the same capture as
    item 18: the numbers `RESPONSE_TIMEOUT_MS` was chosen against came from
    host-batched API log timestamps that could not resolve them. It emits no
    bytes and changes no state machine; with no callback registered (the
    default) it costs only the `millis()` snapshots the engine already takes.

    The first field is deliberately the loop gap and not "ENQ observed to frame
    written": the same-loop send in item 18 puts both of those in one iteration
    and `_currentMillis` is sampled once per iteration, so that delta is
    identically 0 by construction and measures nothing. The ENQ's true arrival
    time is not observable -- it is read out of a UART buffer -- so the gap
    since the previous iteration is the honest upper bound on how stale the ENQ
    was when we answered it, which is the quantity that actually decides whether
    the KW-family sync window is being kept.

21. **GWG burst (behavioral divergence, throughput).** `_init()` may now send a
    queued request on a sync it already earned, without waiting for the device's
    next ENQ, for up to `ENQ_VALIDITY_MS` (1500 ms). Upstream, and the openv
    wiki, wait for a fresh `0x05` every time -- which caps the poll rate at one
    datapoint per ENQ interval (749-2070 ms on the 2026-08-23 capture).

    Sourced from the implementations that have run on hardware: vogod uses
    exactly this 1500 ms ENQ validity on KW and re-stamps it at the end of every
    completed transaction; dannerph has an explicit GWG burst mode; and this
    engine's own `VS1Engine::_syncRecv()` already re-sends with no fresh ENQ
    (see item 22), so GWG was the outlier.

    The KW half is **hardware-confirmed** (2026-08-24, VScotHO1_72 `0x20CB`):
    nine ENQ-delimited chains, median 29 telegrams per ENQ, max 49, with the
    `0x01` prefix on the first telegram of each chain and no other. A 60 s poll
    cycle completes in one chain, 29 datapoints in ~2.15 s. That is the sibling
    protocol, not GWG, so it shows the family tolerates bursting rather than
    proving the GWG device does.

    Deliberately self-correcting, because it contradicts the only written spec:
    a pending ENQ always wins (the burst path is reached only with an empty
    interface, so nothing buffered can be read as a response); the window is
    stamped only by an ENQ actually used to send or by a completed transaction;
    any TIMEOUT invalidates it; and two consecutive unanswered burst sends
    disable bursting for the session with a warning.
    `GWGEngine::BURST_ENABLED = false` compiles it out. Host-proven by
    `tests/native/proof_gwg_burst.cpp`, which passes in both branches.

22. **VS1 sync windows split (structural + tuning).** Upstream, and this copy
    until now, used ONE 50 ms `SYNC_WINDOW_MS` for two unrelated jobs:
    `_syncEnq()`, how fast the master must answer the device's ENQ (a protocol
    constraint), and `_syncRecv()`, how long a chain of telegrams may pause
    before giving up on its sync (a throughput policy). Conflating them pinned
    the second to the first.

    Split into `ENQ_ACK_WINDOW_MS = 50` (unchanged, upstream's value) and
    `CHAIN_WINDOW_MS = 1500`, matching vogod's figure for the same job and
    GWG's `ENQ_VALIDITY_MS`. Motivated by measurement: on the 2026-08-24
    VScotHO1_72 capture the answer-to-next-request gap ran 22-42 ms over 235
    transactions (median 29, p90 35), i.e. ~8 ms under the old 50 ms ceiling.
    Nothing breached it there, but any extra loop latency would, and the effect
    is silent -- the chain just ends and the poll rate drops back to one
    telegram per ENQ (~1.2-2 s each).

    `_syncRecv()` also gained the guard that makes a long window safe: if any
    byte has arrived since the last answer it returns to INIT instead of
    chaining, because on an idle KW bus that byte is the device's next ENQ and
    sending across it would leave it buffered for `_receive()` to read as the
    next request's payload. The guard also bounds the window in practice -- it
    can only be ridden while the device has not spoken since the last answer,
    which is exactly when the old sync is still current. The same reasoning
    applies to `GWGEngine::_init()`, whose burst path now likewise runs only on
    a genuinely empty interface. Host-proven by
    `tests/native/proof_vs1_chain.cpp`.

23. **Host clock made injectable (test-only, no device path touched).**
    `optolink_millis()`'s `__linux__` branch -- which exists solely so
    `tests/native` builds -- now resolves to an inline function behind a
    freezable clock (`optolink_test_clock_freeze` / `_advance` / `_release` in
    `helpers.h`) instead of expanding `std::chrono` inline. The `ESP_PLATFORM`
    and Arduino branches are byte-for-byte unchanged, so no device build sees
    any of it.

    Why: every engine here is a `millis()`-deadline state machine, and the
    proofs were advancing those deadlines with `std::this_thread::sleep_for`.
    That makes the proof race the thing it measures. The tightest case is a
    GWG read, where the response must be pumped within `RESPONSE_TIMEOUT_MS`
    (250 ms) of the send -- two adjacent statements in the test, but not a
    safe bet on a loaded host. `proof_gwg_access_mode.cpp` lost that bet once
    (2026-08-30, host suite run alongside a 6-worker sanitized fuzzing
    campaign): the read timed out, `onTiming` never fired, and the two
    `enq_age` assertions silently compared the PREVIOUS read's leftover
    globals -- reporting as an ENQ-age accounting failure something that was
    only ever a host stall. Reproduced exactly by injecting a 260 ms sleep
    before the pump; 200 ms still passes, 260 ms fails both checks. The engine
    behaved correctly throughout: discarding a response buffered across a
    >250 ms stall is precisely what item 18 exists to do.

    `proof_gwg_access_mode.cpp` is converted: it freezes the clock before
    constructing the engine and advances it by exact amounts, so `enq_age` and
    `send_to_response` are asserted as equalities (0, 30, 120) rather than
    margins, and the run is stall-immune (verified with 400 ms real sleeps
    injected at every deadline boundary) and ~1.7 s faster. The remaining
    proofs still sleep; theirs are mostly one-sided waits that only need to
    OVERSHOOT a deadline, which merely costs time -- but any sleep that has to
    land INSIDE a window (e.g. `proof_vs1_chain.cpp`'s 200 ms against
    `CHAIN_WINDOW_MS`) is the same latent flake and should move to this clock.

    The real-time path also switched from `system_clock` to `steady_clock`:
    every reading is used as a difference against an earlier one, and
    `system_clock` can be stepped backwards mid-run.

Items 7-12, 18, 19, 21 and 22 are the only intentional changes to on-wire/runtime
protocol behavior; items 13-16 are structural (no behavior change from any call
site that exists -- item 16 alters bytes only on a RESPONSE-construction path
nothing exercises); item 17 changes only construction-failure handling
(abort -> fail-soft), not protocol or normal-path behavior; item 20 is a
read-only diagnostic that emits nothing; item 23 is confined to the host test
branch of `optolink_millis()` and cannot reach a device build. Everything else
preserves upstream protocol behavior. Each of items 7-12, 18 and 19 is covered by a host proof
that fails against the upstream code (item 18:
`tests/native/proof_gwg_enq_misread.cpp`, 9 assertions failing pre-fix, plus 4
more for the stalled-host ordering case -- those 4 fail even against a build
that has the deadline but evaluates it after the drain; item 19:
`tests/native/proof_gwg_access_mode.cpp`); item 17's allocation-failure path is
not host-triggerable and rests on inspection plus the wrapper's existing
`begin()` -> `mark_failed()` handling.

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
