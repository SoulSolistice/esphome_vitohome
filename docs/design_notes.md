# vitohome — Design Notes & Engineering Knowledge

The durable *why* behind the component, and the decode/protocol/validation traps
that shaped it. This is the design rationale and the hard-won lessons — not a
user manual or an API table. For the things other docs already own, this file
points rather than repeats:

- **Authoring/decoding a datapoint, the converter table, two-address `select`,
  config-time validation rules** → [`converters.md`](converters.md).
- **Quick start, YAML shape, how it works at a glance** → [`README.md`](../README.md).
- **Exact modifications made to the vendored engine, and licensing** →
  [`optolink/THIRD_PARTY.md`](../components/vitohome/optolink/THIRD_PARTY.md) and
  [`NOTICE.md`](../NOTICE.md).
- **VitoWiFi shortcomings written up as upstream proposals** →
  [`optolink/THIRD_PARTY.md`](../components/vitohome/optolink/THIRD_PARTY.md), Part 2.
- **Catalog generation tooling** → [`scripts/README.md`](../scripts/README.md).
- **The host test harnesses** → [`tests/native/README.md`](../tests/native/README.md).

---

## 0. Source-of-truth discipline (read this first)

Three rules govern every factual claim in this codebase, and they are the reason
the design is what it is:

- **The wire engine is pinned to an exact commit, not a branch.** VitoWiFi is
  vendored at `edc059a7`; an upstream change cannot silently alter decode — or
  OTA — for every device on the next tag. Bumping it is a deliberate act that
  re-runs the validation in §4.
- **The reverse-engineering write-ups are dated.** InsideViessmannVitosoft's
  docs are a map, not the territory — e.g. they describe per-language
  `Textresource_de.xml`/`Textresource_en.xml`, but current Vitosoft ships a
  single `Textresource.xml` keyed by `CultureId`, and `DPDefinitions.xml` now
  wraps its tables in an `ImportExportDataHolder` with an extra
  `DocumentServerDataSet` the docs don't mention (§8). Datapoint facts are
  verified against a *current* export, never recalled from the docs.
- **Claims are labelled by evidence class.** Anything version-sensitive is stated
  as *verified-against-source at `edc059a7`*, not from memory. Findings are
  distinguished as **hardware-confirmed** (replayed against real captures /
  observed on the unit) versus **model-derived** (correct by construction but not
  yet seen on this physical heater). The two are never silently merged.

---

## 1. Architecture — VitoWiFi as a pure wire engine; decode in-component

The vendored engine (`components/vitohome/optolink/`) is used **only** as the
P300/KW/GWG wire layer: framing, sequencing, ACK/NAK, CRC, retries. Every
`Datapoint` is constructed with `optolink::noconv` and writes go through the
raw-bytes overload. **All value decode and encode happens in the component**, in
`decode.h`: read the raw little-endian bytes into a `uint64_t`, scale in
`double`, and narrow to `float` only at the ESPHome state boundary
(`publish_state`). The Python layer turns YAML into the three things the runtime
needs per entity — a `Datapoint(noconv)`, a decode scale, and a signedness flag.

```
YAML ──Python codegen──▶ Datapoint(noconv) + scale + signed
                              │
   bus bytes ◀── optolink engine (frame/CRC/seq) ──▶ raw payload
                              │  (ProtocolAdapter → ResponseView)
                       decode.h (double) ──▶ float32 state
```

This buys a single, host-testable decode/encode path with no hardware in the
loop, and it sidesteps two real hazards in the engine's own converter layer:

### 1a. The `VariantValue` union is tagless

The engine returns decoded values through a **non-discriminated** union over
`_uint8Val`/`_uint16Val`/`_uint32Val`/`_uint64Val`/`_floatVal`. Nothing records
which member was written, so reading the wrong member returns whatever bit
pattern is there — a silent garbage value, not an error. Choosing the correct
member requires knowing the converter that produced it, i.e. exactly the
information the union throws away. Decoding the raw bytes ourselves removes the
guessing entirely.

> The canonical instance of this trap: an earlier revision called
> `operator float()` on a `noconv` value, reading the `float` member over integer
> bytes and publishing silent garbage. That is why the decode path is
> converter-aware rather than "just cast to float."

### 1b. float32 loses bits on 4-byte counters

The engine does converter arithmetic in `float`. A 32-bit counter can exceed
float32's exact-integer ceiling of `2²⁴ = 16,777,216`, after which not every
integer is representable. Burner-hours is the concrete, **hardware-confirmed**
case: a raw seconds value of **212,197,680 s** (`0x0CA5E130`, little-endian
`30 E1 A5 0C`) read at `0x08A7`. The float32 path rounds the raw integer *before*
scaling; the in-component path reads it bit-exact into `uint64_t`, scales in
`double` to **58943.8 h**, and narrows last. The host test asserts both halves:
the bit-exact integer read and the correct scaled hours. (The resulting float32
*state* of ≈59 000.0 is fine — the bug being fixed is the loss that happened
while the value was still a large raw integer.) The same double-then-narrow
discipline is what makes sub-zero temperatures decode correctly under the signed
converters; see [`converters.md`](converters.md) for the preset table and the
signedness rules.

---

## 2. The protocol layer (P300 / KW / GWG)

| Protocol | `protocol:` value | Status |
|---|---|---|
| P300 / VS2 | `P300` (default) | **Hardware-confirmed** on a VScotHO1 (`0x20CB`). |
| KW / VS1 | `KW` | **Hardware-confirmed** on a VScotHO1. |
| GWG | `GWG` | Implemented, **untested** — selectable, ships labelled as needing hardware verification. |

- **4800 8E2 is mandatory.** The hub hard-fails in `validate_uart_()` on any
  baud/data-bit/stop-bit/parity mismatch rather than emitting silent bus errors.
- **Protocol is selected at compile time.** Codegen emits exactly one
  `VITOHOME_PROTOCOL_*` flag from the `protocol:` option (no flag → P300, the
  default and only hardware-exercised path). `protocol_adapter.h` resolves this
  to `SelectedProtocol = optolink::P300 | KW | GWG`.
- **The hub is protocol-agnostic by construction.** `ProtocolAdapter` wraps the
  selected engine and normalises each protocol's differing callback/packet shape
  into a single `ResponseView { data, data_length, address }` (`response_view.h`),
  so the hub registers **one** uniform response handler regardless of which engine
  is built. KW/GWG deliver raw bytes and the request datapoint carries the
  address; P300 surfaces the address in its packet — both collapse to the same
  `ResponseView`. This indirection is what makes adding/maintaining a second
  protocol cheap, and it is exercised host-side without hardware by
  `tests/native/adapter_compile_proof.cpp`.
- **Link is verified at runtime.** After `begin()`, the hub watches for the first
  successful exchange within a window; it logs the established link, or fails
  fast if the configured protocol never establishes one — so a wrong `protocol:`
  surfaces at start-up, not as silent dead air.

---

## 3. The vendored engine (`optolink/`)

The `optolink/` subtree is a **vendored and modified** copy of VitoWiFi at
`edc059a7`, de-branded into `esphome::vitohome::optolink`. The seven intentional
divergences from upstream (the namespace/class rename, removal of the platform
serial adapters, the logging rework, the `std::array` packet buffers, the VS2
write-payload guard, the named timeouts, and the GWG one-shot bugfix) are
itemised in
[`optolink/THIRD_PARTY.md`](../components/vitohome/optolink/THIRD_PARTY.md);
licensing (MIT-into-GPLv3) is in [`NOTICE.md`](../NOTICE.md). They are not
repeated here. What belongs here are the **lessons from doing the vendoring**,
because they generalise:

- **CI green ≠ the vendored engine compiled.** Before the build wiring was fixed,
  CI reported green while still pulling the *old external* library via
  `cg.add_library(...)` — the vendored sources weren't in the build path at all.
  Always verify the *actual* build path, not just the CI badge.
- **ESPHome's component file-copier does not recurse into nested subdirectories.**
  The engine is a nested tree (`protocol/vs2/…`, `datapoint/…`, `interface/…`),
  which the copier won't carry. The fix is to register `optolink/` as a `file://`
  PlatformIO library from the component directory (with `optolink/library.json`),
  so PlatformIO — not the copier — pulls the whole tree on both frameworks. The
  flat-vs-nested layout question is therefore settled in favour of **nested**.
- **`ref: main` without `refresh: 0s` throttles re-pulls to one day.** A same-day
  rebuild after pushing a fix will compile a cached, stale clone. Use
  `refresh: 0s` (or a pinned ref) during active iteration.

---

## 4. Config-time validation is load-bearing, not belt-and-braces

The engine guards converter/length combinations with `assert()`, which is
**compiled out under `NDEBUG`** — i.e. in every ESPHome release build. Worse, the
upstream `noconv` length assert is **commented out** even in debug, so an
out-of-range length would silently decode as `0`.

That is why the converter/length cross-checks in the Python layer are the real
guard: they turn a silent-wrong-data bug into an `esphome config` error before
anything reaches the device. The actual length sets and the encodable-range rule
live in [`converters.md`](converters.md); the principle to carry is that those
checks are **load-bearing** and must stay in lockstep with `decode.h`. The
config-time encodable-range check in `number.py` mirrors
`decode.h::encode_scaled` *exactly* (round to nearest raw step, then range-check
for byte width and sign), and the C++ `encode_scaled` itself range-checks
unconditionally before transmitting — belt **and** braces, on purpose, precisely
because the engine's own runtime guards vanish under `NDEBUG`.

**When bumping the engine commit or adding a converter:** re-read the engine's
converter source at the new revision, update the length sets to match, and add a
`tests/unit/test_validators.py` case. The asserts there are the source of truth;
do not infer the constraints.

---

## 5. The write path

`number` and `select` stage a raw payload (`encode_scaled`, or a little-endian
enum value) into the entity's buffer and call `request_write(this)` on the hub.
The hub keeps separate read and write deques and **writes preempt reads**, so a
user setpoint change doesn't wait behind a full poll cycle. On the device ACK,
with `read_back: true` (default) a read of the same address is pushed to the
*front* of the read queue, so Home Assistant reflects the device's own view
rather than an optimistic guess; with `read_back: false` the entity publishes
optimistically on ACK.

The state model here is subtle and was the source of a real bug:

- **`write_queued_` and `write_in_flight_` are distinct states**, and conflating
  them silently loses writes. `write_queued_` means "sitting in the hub's write
  queue, awaiting dispatch"; `write_in_flight_` means "dispatched to the engine,
  awaiting ACK/error." An entity whose write is already in flight must still be
  able to **re-enqueue** a newer value (`write_queued_ = true`) even though the
  in-flight transaction can't be recalled. Every completion path — dispatch,
  write-ACK, mismatch-drop, error, watchdog — clears `write_in_flight_`
  independently of `write_queued_`. (An earlier single-flag design dropped the
  second write whenever `control()` fired during an in-flight transaction.)
- **`pending_value_` can desync Home Assistant.** Publishing `pending_value_`
  while a write is in flight can show a state the device hasn't accepted; it must
  be managed together with `write_in_flight_`/`read_back`, not published blindly.
- **Mode controls are two-address.** Some controls (e.g. operating mode) read
  their live value at one register and accept commands at another — `address:` is
  the write/command address, optional `state_address:` is the read/state address;
  polling, read-back and response-matching use the state address. The user-facing
  shape is documented in [`converters.md`](converters.md). One **hardware
  caveat**: whether a mode write is reflected on read-back is governed by the
  device (program-switch position, register read/write asymmetry), not by the
  component — the write→ACK→read-back pipeline is correct at the software level
  but the unit may legitimately report an unchanged value. Confirm mode behaviour
  on the actual heater rather than assuming the software ACK means the mode
  changed.
- **`select` uses the index-only `control(size_t)` override** preferred by
  ESPHome 2026.5.0 (the string variant forwards to it; `Select::state` is
  deprecated/removed in 2026.7.0). Options are an **ordered `{raw_value: label}`
  map**, so the YAML author controls order; raw values are validated to fit the
  byte width and labels to be unique.
- **The raw lane is not uniformly higher-priority than `write_queue_`.**
  `dispatch_next_()` used to check `raw_queue_` ahead of `write_queue_`
  unconditionally, which was fine while the raw lane only carried
  `RawPurpose::SCAN` (interactive scan-console ops, which *should* jump ahead
  of a queued write so range sweeps feel immediate). Once system-time sync
  started riding the same lane (`RawPurpose::CLOCK_READ/CLOCK_WRITE/
  CLOCK_VERIFY`), that same unconditional priority let a background clock
  sync — up to three sequential round trips, invisible to the user — stall a
  queued setpoint write for the whole sync. `dispatch_next_()` now checks the
  front op's purpose: `SCAN` still preempts `write_queue_`; `CLOCK_*` is
  checked *after* `write_queue_` instead, so a pending user write always goes
  out first. One known residual: because `raw_queue_` is a single FIFO,
  a `SCAN` op enqueued *behind* a not-yet-dispatched `CLOCK_*` op still has to
  wait for that `CLOCK_*` op's own turn (which itself now waits on
  `write_queue_`) — the priority split is by the queue's front purpose, not a
  full reordering of the deque. In practice this window is narrow (a clock op
  is enqueued one step at a time, only once its predecessor's response has
  landed) and self-resolving within one or two dispatch cycles; a real fix
  would give `CLOCK_*` its own lane instead of sharing `raw_queue_`.

---

## 6. Device identification — `0x20CB` is only the *family*

With `identify_device: true` (default), the hub runs a small **fail-soft** state
machine after `begin()` and before regular polling: one length-4 read at
`0x00F8`, falling back to four length-1 reads at `0xF8/F9/FA/FB` if the block
read comes up short. Each field that can't be read is left unknown; the result
(`group`, `controller`, `hw`, `sw`) is logged at INFO and pushed to any
`device_id` text sensors. A small built-in table names the families seen on the
wire (`0x20CB` → VScotHO1, plus `0x2098`/`0x2094`/`0x2053`); everything else is
reported as raw hex.

The load-bearing fact for anyone **adding new datapoints**:

- **A unit is identified by a tuple, not the 2-byte ID.** `0x20CB` = group `0x20`
  / controller `0xCB` = the *family* `VScotHO1`. The software distinction
  (`VScotHO1` vs `VScotHO1_72`, "ab Softwareindex 72 / Projekt Neptun") is the
  **software index at `0xFB`** — it is *not* encoded in `0x20CB` and cannot be
  recovered by decoding `0x20CB` differently.
- **`VScotHO1` is a Vitosoft internal token, not a marketing name.** The 2026
  export carries no product names (§8), so the token alone does not tell you the
  product. On the reference unit `VScotHO1_72` ("Projekt Neptun") is the
  **Vitotronic 200 controller in a Vitodens 300-W (type B3HA)** — corroborated by
  the `Neptun_Durchfluss_*` / `Neptun_Volumenstromgrenzwert_*` flow-sensor
  datapoints in its catalog, a Vitodens-300-W feature. The fault-code default is
  therefore the Vitodens 300-W B3HA set (`fault_codes.VITOTRONIC_VD300_B3HA`),
  not the Vitodens 200 one.
- **Authoring a new datapoint from the Vitosoft export → match your unit's
  `0xFB`.** Address and conversion can differ between `VScotHO1` and
  `VScotHO1_72`; picking the wrong variant row is a **silent-wrong-decode trap**.
  (Existing datapoints pinned to bytes this physical unit returned are immune —
  the wire is ground truth and the software index is fixed at runtime.)
- **Any future auto-identify feature must match the full tuple** (`0xF8`–`0xFB`,
  optionally `0xF0`), never `0x20CB` alone, or it loads the wrong datapoint set
  for any family with software-index variants. The `0xFA`/`0xFB` reads on this
  unit are **model-derived** — verify on a first run.

---

## 7. Error-history layout (corrected)

The old `esphome_vitoconnect` config read 3 bytes at `0x7507` as
`[code, day, month]`. That layout was a guess and is **wrong**. The authoritative
layout comes from InsideViessmannVitosoft's own decoder
(`Viessmann2MQTT.py`, `DateTimeFromBCD`): a slot is **9 bytes** — `[0]` = error
code, followed by an 8-byte packed-BCD timestamp:

| byte | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|------|---|---|---|---|---|---|---|---|---|
| field | code | year-hi | year-lo | month | day | weekday* | hour | minute | second |

\* weekday is `sunday = 0` (strftime %w; hardware-confirmed) and is ignored on
decode.

`decode.h::decode_datetime_bcd()` validates that every nibble is BCD (so
`0xFF`-filled empty slots fail cleanly), range-checks the fields, and rejects
implausible years (`< 1990`) so an all-zero slot doesn't decode as year 0. The
`error_history` text sensor emits `"<label> (0x<code>) @ YYYY-MM-DD HH:MM:SS"`,
falling back to code-only when the timestamp is empty/invalid.

**Error history is catalog-driven, not a single hardcoded address.** A unit may
not keep its fault log at `0x7507` at all: VScotHO1_72 exposes its log as the
per-slot datapoints `FehlerHisFA01..20` at `0x7590..0x763B` (9 bytes each) and
has **no** event at `0x7507`. `gen_catalog.py` therefore emits one
`error_history` entity per `FehlerHis*` slot at its own address — `FA01` →
"Letzter Fehler", `FA02..20` → "Fehler NN" (disabled by default) — all under
`entity_category: diagnostic`. When such per-slot datapoints exist they are
authoritative and the generic `ecnsysEventType~Error` / `0x7507` slot is
**suppressed**, so there is exactly one "Letzter Fehler". The `codes:` map is
selected with `--error-code-set` from `scripts/fault_codes.py` (the single
source of truth: `openv`, `vd200` (manual-verified, default), or `union`;
openv-vs-VD200 disagreements are kept in `fault_codes.CONFLICTS`, not
overwritten — see `NOTICE.md`). It maps
the display-Stoerungscode space (byte[0]) only, not the GFA byte (`0x5738`), the
LON alarm record, or the self-describing sensor-status enums.

> **Hardware caveat (unresolved).** A **9-byte block read** per slot (the
> `FehlerHis*` addresses above, or a block at `0x7507` on units that use it) is a
> different transaction than the per-byte reads that NAKed on the reference unit,
> and is how Viessmann2MQTT reads it — but it must be confirmed on the actual
> unit. If the device NAKs the block read, drop `length:` to 1 for a code-only
> sensor.

---

## 8. The Vitosoft data export (2026, empirically verified)

Per-device datapoint definitions are **not discoverable over the bus** — they
live in Viessmann's Vitosoft data, surfaced by the InsideViessmannVitosoft
scripts and consumed by `scripts/gen_catalog.py` (see
[`scripts/README.md`](../scripts/README.md) for the workflow). The 16-bit
Optolink address is the hex **after the `~`** in an event address string
(`Outside_Temp~0x0800` → `0x0800`); the token before is just the internal name.

Concrete shape of a full 2026 export, recorded in case it shifts again:

- **`DPDefinitions.xml`**: ~203 MB, UTF-8 with BOM. One `ImportExportDataHolder`
  containing one `ECNDataSet` (~589k rows across 20 tables) **plus** a
  `DocumentServerDataSet` (mobile-client / error-code extensions). Parses in ~27 s
  at ~1.6 GB peak via a DOM; prefer a streaming parse on a low-RAM target.
- **`Textresource.xml`**: UTF-16 LE, consolidated multi-culture (15 cultures,
  `de` = CultureId 1) — but **UI strings only**. `Textresource_de.xml` is a
  byte-identical copy, not a German subset.
- The installer ships **stale** XML; it is regenerated from the embedded SQL
  database on first launch of Vitosoft. Verify against a freshly-generated export.

### The operational catch: display names are not in the offline export

Every event, value and group references its display name via an
`@@viessmann.*.name.*` label, but the strings those labels resolve to are
**absent from the 2026 export** — Textresource carries none of them, and there is
no localization table anywhere in DPDefinitions (only `ecnCulture`, the language
list). They were most likely moved to Viessmann's cloud/DocumentServer at
runtime; only ~125 system/RPC events carry real localized names. What every
datapoint *does* still carry is the full technical spec: a stable technical
identifier (`Outside_Temp`, …; ~10,781 of 11,582 are clean ASCII tokens) plus
address, length, conversion, FCRead, access mode, the value/enum structure,
units and borders.

**Naming strategy — drive the two ESPHome fields independently:**

- **`id:` ← technical identifier.** Deterministic, ASCII, stable across firmware;
  the join key back to the Viessmann data. Always carried, even when a friendly
  name exists.
- **`name:` ← friendly name.** Use a real translation where available; otherwise
  derive from the id by turning `_` into spaces. The derivation is deliberately
  light (cleans snake_case, leaves camelCase / coding-prefix compounds intact),
  so a real localization source (the openv wiki, or a recovered cloud one) is
  still preferable where clean labels matter. The catalog generator always prints
  the technical id alongside the friendly name, so the id is never lost.

**Enum *option* labels are a separate, mostly-present source.** The above is about
entity *names*. The readable text for individual enum **values** lives in
`ecnEventValueType.Description` (a pre-resolved string needing no Textresource
lookup — e.g. `0x00` → "OK", `0x02` → "Unterbrechung"), which the generator now
reads. ~87% of this device's enum option values carry a Description; the rest are
genuinely blank in the export and fall back to the resolved token stem then hex.
So an enum/`select`'s `options:` are human-readable even though the entity name
beside them may be a derived snake_case label. (The width an enum option reads is
derived from its values, not the block length, so a 1-byte status field inside a
larger block reads 1 byte at its `BytePosition`.)

---

## 9. Per-entity polling

The hub has a base `update_interval` (the poll tick). Each entity may set its own
`update_interval`, honoured at **hub-tick granularity** — so an entity interval
shorter than the hub interval silently degrades to the hub interval. Rather than
let that surprise anyone, `setup()` logs a warning naming each such entity.
(There is intentionally no `final_validate` cross-check between entity and hub
intervals: the hub id isn't reliably resolvable from a platform's
`final_validate` without brittle global lookups, so the runtime warning is the
chosen tradeoff.) Suggested tiers: live measurements ~60 s, monotonic counters
~600 s, writable coding values ~3600 s, error history ~300 s.

---

## 10. Testing and the gate model

The gates are **sequential and non-substituting** — passing an earlier gate does
not vouch for a later one:

```
host C++ : decode/encode tests           tests/native/test_decode.cpp   (380 checks)
host C++ : VS2 transaction harness        tests/native/test_vs2_transaction.cpp (8/8)
host C++ : adapter / GWG compile-proofs   adapter_compile_proof.cpp, proof_gwg_poke.cpp
python   : validators + catalog generator tests/unit/  (pytest)
lint     : ruff check / ruff format
format   : clang-format  (pinned v19.1.4)
config   : esphome config   (both test YAMLs)
compile  : esphome compile  (esp-idf AND arduino)
run      : esphome run       (real heater — the definitive gate)
```

Two coverage layers compose to give wire→decode→value end-to-end without the
ESPHome framework:

- **`test_decode.cpp` (380 checks)** locks down the *value* layer — the
  precision fix, BCD/datetime, sign-extend, encode round-trips, `raw_fits`
  boundaries.
- **The VS2 transaction harness (8/8)** locks down the *transaction* layer the
  upstream library never unit-tested — request/ACK/response and, critically,
  **fragment reassembly** (both `0x23xx` reads arrive split on hardware). Its
  fixtures are golden-master vectors lifted from live firmware captures and
  recompiled against the actual in-tree engine; the write vectors are
  hardware-confirmed Betriebsart switches. Detail and the vector table are in
  [`tests/native/README.md`](../tests/native/README.md).

Three findings the transaction build surfaced, each now a permanent lesson:

1. **Namespace collides with the umbrella class name** — it must be fully
   qualified; `using namespace` is ambiguous. (Resolved at the source by renaming
   the umbrella template to `OptolinkEngine` during vendoring; see THIRD_PARTY.md.)
2. **Packets were non-copyable** while they owned a `malloc` buffer, so the
   callback extracts the payload rather than copying. The `std::array` buffer swap
   makes them trivially copyable again — the copy operators are now restored.
3. **A write-ack's `data()` is `nullptr` by design, but `dataLength()` still
   returns the echoed length** — a naive `data()[0..dataLength())` segfaults (ASan
   caught it instantly). The correct consumer guard is `if (data())` before
   reading payload, which both the harness and the hub use. This is the exact
   non-discriminated-read class the architecture exists to avoid, now
   machine-checked.

Two limits worth re-stating because they are recurring footguns:

- **`esphome config` ≠ `esphome compile`.** Config validates schemas and runs the
  config-time validators but does **not** run `to_code`/codegen; a generated
  method-name mismatch only shows up under a full compile.
- **No host test substitutes for `esphome run` on *address* correctness.** The
  catalog and reference YAML get the *shape* right; only the wire confirms that a
  given address answers on a given firmware.

---

## 11. Open items / forward work

- **GWG hardware verification.** GWG is implemented and host-proven but never run
  against a GWG unit; it ships labelled as such, gated on a unit or a community
  tester. KW/VS1 and P300 are hardware-confirmed.
- **`Convert4BytesToFloat`** (IEEE-754 datapoints) is not yet a converter; such
  datapoints are surfaced by the catalog generator as commented hints rather than
  decoded wrongly. Note this is *not* the same as `sec2hour`, which reads 4 bytes
  as a `uint32`. (`RotateBytes` and `HexByte2UTF16Byte` were previously in this
  commented-hint set and are now handled — the big-endian `rotatebytes` converter
  and the `type: utf16` text_sensor, both host-tested in `test_decode.cpp`. Per
  §4, the `rotatebytes` preset still wants a `tests/unit/test_validators.py` case.)
- **ESP32 build has two independent axes worth not conflating:**
  `esp32.framework.type` (`arduino`/`esp-idf` — which runtime SDK the code is
  compiled against; every test config here already uses `esp-idf`) and
  `esp32.toolchain` (`platformio`/`esp-idf` — which *build system* orchestrates
  the compile). The second one is what the rest of this entry is about.
  - **What happened.** ESPHome 2026.5.0 shipped a native ESP-IDF toolchain
    that drives `idf.py`/CMake directly instead of wrapping PlatformIO,
    opt-in alongside the existing default. ESPHome's `dev` channel has since
    flipped the *default* from `platformio` to `esp-idf` (confirmed not yet
    in `beta` or stable). That broke the `upstream-canary` workflow: under
    the native toolchain, our vendored optolink engine's `file://`
    PlatformIO-library registration
    (`components/vitohome/__init__.py::to_code`) gets routed through
    ESPHome's PlatformIO-library-to-IDF-component converter
    (`esphome/platformio/library.py::convert_libraries`), which treats *any*
    non-empty `repository` string as a git remote — no `file://` case — and
    tries (and fails) to `git clone` a local directory.
  - **Two separate, complementary fixes, not one.**
    (1) `tests/test.esp32-idf.yaml` / `tests/test.esp32-arduino.yaml` pin
    `toolchain: platformio` explicitly — a no-op against the pinned build
    and `beta` (both already default to it), and it restores the working
    path on `dev`. This makes the canary test what this project actually
    ships, not an unrelated upstream default change.
    (2) `esp32.toolchain: esp-idf` is *also* now genuinely supported, not
    just avoided — `to_code` has a toolchain-conditional branch:
    `esp32.add_idf_component(name="optolink", path=optolink_dir)` (writing a
    real, standard ESP-IDF Component Manager `path:` dependency) when
    `CORE.using_toolchain_esp_idf`, the old `file://` library registration
    otherwise. The vendored engine's nested `protocol/`/`datapoint/`
    subtrees were never going to reach either build "for free" regardless of
    toolchain — ESPHome's own component file copier
    (`loader.py`'s `ComponentManifest.resources`) only copies files sitting
    directly in a component's top-level directory, which is why the engine
    needed the `file://` trick under `platformio` in the first place. A new
    `optolink/CMakeLists.txt` is the CMake-side twin of `optolink/library.json`
    for the native toolchain (ESP-IDF's build doesn't read PlatformIO
    manifests at all). One real bug was caught building this, not just
    anticipated: the project-wide `-I<component_dir>` flag that makes
    `#include "optolink/optolink.h"` resolve is silently dropped under the
    native toolchain (`build_gen/espidf.py::get_project_cmakelists`, pinned
    2026.6.2, only propagates `-D`/`-W` flags project-wide), so
    `optolink/CMakeLists.txt` exposes its own parent directory via
    `INCLUDE_DIRS ".."` instead — "main" already implicitly `REQUIRES`
    optolink via the `path:` dependency, and ESP-IDF auto-propagates a
    required component's public `INCLUDE_DIRS` to the requiring component.
  - **Validation.** (1) is config-level and trivially verified. (2) is
    validated by two full, successful native compiles in sandbox — both
    `framework.type: esp-idf` and `framework.type: arduino` combined with
    `toolchain: esp-idf`, each ending in "Successfully compiled program."
    with real `firmware.factory.bin` output, not just codegen succeeding.
    Reproducible with `esphome compile tests/test.esp32-idf-native.yaml`.
    Neither has been run on real hardware, and nothing selects
    `toolchain: esp-idf` in any shipped config today — treat "compiles
    cleanly" as exactly that claim. `esp32.add_idf_component(path=...)` has
    no other call site anywhere in ESPHome's own component tree as of this
    writing (every other user of that function passes `name=`/`ref=` for a
    registry or git component) — the mechanism is real, documented ESP-IDF
    functionality and now proven by two clean builds here, but this project
    is among the first to exercise this exact path inside ESPHome's codegen.
  - **Not wired into any CI workflow.** `tests/test.esp32-idf-native.yaml`
    is manual/local-only for now. If/when `esp32.toolchain: esp-idf`
    actually reaches a real ESPHome release, that's the point to decide
    whether it also becomes a real CI leg.
  - **Not yet filed upstream.** No existing esphome/esphome issue for the
    `file://`-as-git gap was found when checked. The surrounding module is
    under active development — a closely related git-ref bug in the same
    code path was fixed in a PR the same week this was diagnosed — so this
    looks like a genuine, recently-introduced, so-far-unreported gap.
