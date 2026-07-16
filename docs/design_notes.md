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
                              │  (byte-mover callback → ResponseView)
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
guessing entirely -- and the vendored engine has since deleted `VariantValue`
and the converter decode/encode layer outright (`THIRD_PARTY.md` items 13/15),
so the hazard class no longer exists in-tree; this section records why the
decode path was designed this way.

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
  default and only hardware-exercised path). `protocol_select.h` resolves this
  to `SelectedProtocol = optolink::P300 | KW | GWG`.
- **The hub is protocol-agnostic by construction.** All three engines speak
  one byte-mover API — `read(address, length)` / `write(address, data, length)`
  and callbacks delivering `(data, length, address)` — so the hub drives
  `OptolinkEngine<SelectedProtocol>` directly and wraps each response in a
  single `ResponseView { data, data_length, address }` (`response_view.h`);
  the entities never see the engine callback shape. On P300 the `address`
  field is the one **echoed in the response frame itself** (the device echoes
  it on read responses and write acks alike — hardware-confirmed by the
  transaction-harness fixtures), so the hub's response-address match is a live
  wire-level check there; on KW/GWG no address travels in the response, the
  engine echoes the request's, and the check degenerates to a tautology. The
  uniform engine API is what makes adding/maintaining a second protocol cheap,
  and it is exercised host-side without hardware by
  `tests/native/engine_compile_proof.cpp` (deliberately compiled without the
  datapoint/converter translation units — the engine layer has no Datapoint
  dependency).
- **Link is verified at runtime.** After `begin()`, the hub watches for the first
  successful exchange within a window; it logs the established link, or fails
  fast if the configured protocol never establishes one — so a wrong `protocol:`
  surfaces at start-up, not as silent dead air.

---

## 3. The vendored engine (`optolink/`)

The `optolink/` subtree is a **vendored and modified** copy of VitoWiFi at
`edc059a7`, de-branded into `esphome::vitohome::optolink`. Every intentional
divergence from upstream -- from the namespace rename and the `std::array`
packet buffers through the protocol-level bugfixes and the dead-code sweeps --
is itemised, numbered and classified (behavioral vs structural) in
[`optolink/THIRD_PARTY.md`](../components/vitohome/optolink/THIRD_PARTY.md);
licensing (MIT-into-GPLv3) is in [`NOTICE.md`](../NOTICE.md). That file is the
single source of truth for the divergence list: a count and summary previously
repeated here drifted out of date as items were added, so neither is repeated
anymore. What belongs here are the **lessons from doing the vendoring**,
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

`number`, `select` and `switch` stage a raw payload (`encode_scaled`, or a
little-endian enum/boolean value) into the entity's buffer and call
`request_write(this)` on the hub.
The hub keeps separate read and write queues (fixed-capacity ring buffers — see
§5a) and **writes preempt reads**, so a
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
- **The raw lane is uniformly higher-priority than `write_queue_` again — and
  that is now correct.** History, because the shape of the fix matters:
  `dispatch_next_()` originally checked `raw_queue_` ahead of `write_queue_`
  unconditionally, which was fine while the lane only carried
  `RawPurpose::SCAN` (interactive scan-console ops, which *should* jump ahead
  of a queued write so range sweeps feel immediate). Once system-time sync
  started riding the same lane (`RawPurpose::CLOCK_*`), that unconditional
  priority let a background clock sync — up to three sequential round trips,
  invisible to the user — stall a queued setpoint write for the whole sync. The
  fix at the time split the priority by the *front op's purpose*: `SCAN` still
  preempted `write_queue_`, `CLOCK_*` was checked after it. That left a known
  residual, since a single FIFO cannot reorder: a `SCAN` enqueued *behind* a
  not-yet-dispatched `CLOCK_*` still waited for it. The note ended by saying a
  real fix would give `CLOCK_*` its own lane.
  It got something better: clock sync is no longer a raw op at all. `VitoClock`
  is an entity on the read/write lanes (see the raw-lane sizing section above),
  so `raw_queue_` has exactly one tenant, the purpose tag and the arbitration it
  required are both gone, and the unconditional `SCAN`-preempts-writes rule is
  restored with nothing left to be wrong about. Writes still preempt reads, so
  the clock's own write step is not starved either.

### 5a. The three lanes are sized-at-setup ring buffers, not deques

`read_queue_`, `write_queue_` and `raw_queue_` are `RingBuffer<T>`
(`components/vitohome/ring_buffer.h`), not `std::deque`. A deque mutated on every
tick — which all three are — allocates and frees as it grows and drains: heap
churn *after* `setup()`, the one thing the memory-discipline rules forbid on the
C3. The ring makes **exactly one allocation per lane, in `reserve()` at
`setup()`, and never again**; every push/pop is O(1) with no allocation, so
run-loop traffic can't fragment the heap. It is double-ended because the write-ACK
read-back (above) prepends to the front of the read lane, so plain FIFO wouldn't
do.

**Why each lane is sized to its real need at `setup()` rather than a compile-time
cap.** `register_entity()` registers *every* defined entity, and the scheduler
polls all of them — `disabled_by_default` is a Home Assistant hint the device
ignores (§9). So `entities_.size()` is the *total defined* count, and the
generated catalogs under `example/` are meant to be `packages:`-included whole,
ranging into the thousands of datapoints. A fixed compile-time ceiling would
hard-fail a legitimate full-catalog config **at boot while `esphome config`
passes** — a config-valid-but-runtime-dead split. So the read/write lanes are
`reserve()`d to `entities_.size()` (their true ceiling: the `read_queued_` /
`write_queued_` flags admit an entity to a lane at most once, so it can never
fill), and the raw lane to `raw_queue_capacity_` — the YAML `raw_queue_size`,
defaulting to `RAW_QUEUE_DEFAULT`. One boot-time allocation each; no arbitrary
ceiling; no wasted headroom.

The raw lane is the interactive scan console and nothing else, and it defaults
to **0**: a debug tool should not tax every config, and on an ESP8266 (~40 KiB
heap) a sweep-sized lane is a quarter of the budget. Scanning is opt-in — size
`raw_queue_size` to the largest burst intended (1–2 for one-off
`queue_raw_read()` presses; `example/vitohome-scanner-raw.yaml` uses 256,
~9.7 KiB, for its RANGE SWEEP). An enqueue against an unallocated lane is
rejected with a warning naming the option, so the failure is loud.

That depth **cannot be derived from the config**: the lane is driven through
`queue_raw_read()`/`queue_raw_write()` from lambdas, and the shipped sweep's
count is a Home Assistant action parameter chosen at runtime, so no
`FINAL_VALIDATE_SCHEMA` inspection could infer it. Hence an explicit knob rather
than an auto-size.

**Device-clock synchronization does not use this lane.** It used to, tagged with
a `RawPurpose`, which coupled two unrelated tenants in one queue and forced a
non-zero default. It is now `VitoClock` (`vito_clock.h`) — a hub-owned
`VitoEntityBase` on the ordinary read/write lanes. The fit is exact rather than
forced: `write_buf_` is 8 bytes and the clock datapoint (0x088E) is 8 bytes of
BCD; `handle_response()` is the drift compare; and `wants_read_back()` — already
true by default — makes the verify step the existing write-ACK read-back rather
than bespoke code. `VitoEntityBase` inherits from nothing (the concrete types
multiply-inherit their Home Assistant surface separately), so a pure subclass is
a lane participant with no HA presence, which is exactly what clock sync is.

Two failure modes disappeared with the move. A sweep filling the raw lane could
starve a mid-chain clock write; and `raw_queue_size: 0` silently disabled time
sync, which is why a validator once rejected that pair. The entity lanes are
`reserve()`d to `entities_.size()` and the `read_queued_`/`write_queued_` flags
admit an entity at most once, so a slot for the clock always exists. Pinned by
`tests/native/proof_ring_buffer.cpp::test_clock_chain_on_entity_lanes`.

**The clock address is a device property, not a constant** — source-confirmed
against the Vitosoft `DPDefinitions.xml` link tables (399 `ecnDatapointType`
tokens, 104,339 `ecnDataPointTypeEventTypeLink` rows). Three schemes exist:

| Scheme | Address | Tokens |
|---|---|---|
| `NRF_Uhrzeit` | 0x088E, 8-byte `DateTimeBCD` | Ecotronic, VBC550P, VBC550S |
| `WPR_Uhrzeit` | **0x08E0**, 8-byte `DateTimeBCD` | V200WO1A, VBC700_AW, VBC700_BW_WW, VBC702_AW, VBC702_S, CU401B_A/G/S (heat pumps) |
| `GWG_Uhrzeit_*` | 0x0074/0x0075/0x0076, three 1-byte registers | GWG_VBES_00/03/21/35/36 |

Hence `time_sync: clock_address:`, defaulting to 0x088E. WPR is the case that
makes it necessary: 0x08E0 and 0x088E are both valid 16-bit addresses, so
nothing would reject anything — the component would read 8 bytes from a
datapoint that isn't the clock, compute nonsense drift, and then **blind-write a
BCD timestamp to it**. Only the address is configurable: both `DateTimeBCD`
variants are 8 bytes, and 8 is exactly `write_buf_`.

GWG is not served by that option, because its clock is a different *shape*, not
a different address — so `_final_validate` rejects `time_id` under `protocol:
GWG` outright. (The default 0x088E is over GWG's 8-bit space anyway, and
`PacketGWG::createPacket()` rejects it — loudly, once per sync interval, hours
apart, which is exactly the kind of quiet breakage a config-time error is for.)

**The XML is not a complete authority here, and the option is shaped around
that.** Only **16 of the 399** tokens list any clock datapoint at all — and
`VScotHO1_72`, the reference Vitodens 300-W, is *not* among them, despite 0x088E
being hardware-confirmed on it. So the XML is authoritative that 0x08E0 exists
and differs; it is *not* authoritative that 0x088E is correct for the other 383.
That asymmetry is why this is a user-set option with a known-good default rather
than a lookup keyed on the device ident: such a lookup would answer "unknown"
for the overwhelming majority of real devices, including the one this component
was developed against. `gen_catalog.py` skips `DateTimeBCD` entirely, so the
clock is never a catalog datapoint either way.

Two details make the move behaviour-preserving rather than merely tidy. The raw
lane was dispatched **ahead** of the poll lane; an ordinary polled entity is
not, so the clock would have queued behind every pending poll (~150 s on a full
catalog — harmless for drift, which is measured against the live time source at
response time, but a visible regression for `sync_on_boot`). So `VitoClock`
opts out of the poll rotation (`wants_polling() == false`) and pushes to the
**head** of the read lane via `request_priority_read()` from its own schedule.
And because the read lane cannot fill, a second sync could no longer be rejected
by a full queue the way the depth-1 raw lane implicitly rejected it — so the
chain guards itself with an explicit `Phase` (`IDLE`/`READING`/`VERIFYING`),
which is `RawPurpose::CLOCK_*` moved out of the shared lane and made local to
its only user.

**The API is deliberately not the deque subset.** `reserve()` is **one-shot**
(a second call is rejected, `reserve(0)` permanently initializes an unused
lane; a *failed* allocation leaves it uninitialized so `setup()` can
`mark_failed()`), and a reference-returning `front()`/`pop_front()` pair is not
exposed — a reference could escape the lock, and a separate peek-then-pop is a
race under concurrency. Instead: `try_front()`/`try_pop_front()` copy under one
lock, and **`consume_front_if(consumer)`** implements the dispatch hand-off —
it holds the lock while the hub offers the front item to the protocol engine
and removes it only if the engine accepted (`EMPTY`/`RETAINED`/`REMOVED`). The
consumer must not call back into the same ring: the mutex is non-recursive.
Every mutating call is `[[nodiscard]]`; a rejected push **must roll back its
companion bookkeeping** (`read_queued_`/`write_queued_`) rather than leave an
entity marked queued but absent — the scheduler, `request_write()` and the
read-back path all follow that set-flag-then-push-then-roll-back pattern.

**Synchronization scope, stated precisely.** Each ring serializes its own
operations through `esphome::Mutex` (FreeRTOS-backed on ESP32; an inline no-op
on the single-threaded ESP8266/RP2040; the host proofs build with
`-DVITOHOME_NATIVE_TEST`, which selects a no-op stand-in in the header since
ESPHome headers aren't on the host include path). This makes a *single queue
operation* safe against a producer on another FreeRTOS task — which is why
`queue_raw_read()`/`queue_raw_write()` are safe to call from one: `enqueue_raw_`
is a single self-contained `push_back`. It does **not** make the hub's compound
entity-lane invariants (flag *and* queue updated together) multi-task-safe;
entity control paths must stay on the ESPHome loop. Not ISR-safe by design.
Semantics are pinned in `tests/native/proof_ring_buffer.cpp` under ASan/UBSan
(one-shot reserve, FIFO, wraparound, `push_front`, full-rejection,
`consume_front_if` outcomes, POD round-trip, the hub dispatch pattern).

---

## 6. Device identification — `0x20CB` is only the *family*

With `identify_device: true` the hub runs a small **fail-soft** state machine
after `begin()` and before regular polling: one length-4 read at `0x00F8`,
falling back to four length-1 reads at `0xF8/F9/FA/FB` if the block read comes
up short. The default is **on for P300 and KW**, off for GWG — the fallback
path makes identification work on KW's byte-oriented protocol
(hardware-confirmed on `0x20CB` over both P300 and KW: `HW=0x03 SW=0x51`),
whereas GWG's scheme is untested, so it must be enabled explicitly there.
Each field that can't be read is left unknown; the result (`group`,
`controller`, `hw`, `sw`) is logged at INFO and pushed to any `device_id`
text sensors. A small built-in table names the families seen on the
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
source of truth: `openv` (41 codes), `vd200` (59), `vd300` (94, the Vitodens
300-W B3HA = VScotHO1_72 set and the **default**), or `union` (105, all merged,
most-specific manual wins); openv-vs-VD200 disagreements are kept in
`fault_codes.CONFLICTS`, not overwritten — see `NOTICE.md`). It maps
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

### Bit numbering in the export is MSB-first

`BitPosition` counts from the **most significant** bit: index 0 is `0x80`, index 7
is `0x01`. The generator emitted `1 << (bit_pos % 8)` — mirrored for *every*
bit-field it has ever produced.

Hardware proof (VScotHO1_72 / `0x20CB`, 2026-07-09 logs). Address `0x55DD` carries
exactly two datapoints: `GWG_Flamme1` (BitPosition 2) and `GWG_Brenner_2`
(BitPosition 5). The byte reads `0x01` with the burner off (3 samples, modulation
0 %) and `0x21` with it firing (5 samples, modulation 11–40 %, flue gas rising).
Bit `0x04` is never set in either state. LSB-first would therefore mean "no flame
while the boiler burns", and would light a second burner stage this modulating
unit does not have. MSB-first puts `GWG_Flamme1` at `0x20`, which tracks the burn
exactly.

**Independently confirmed on a second register** (2026-07-10 log). `0xA152` is a
2-byte relay bitfield with sixteen documented `BitPosition`s. Read little-endian
(`>>> F7:A1:52:02` → `<<< 04:00` → `4`), the four observed states decode
MSB-first into a coherent picture and LSB-first into nonsense:

| raw | MSB-first | corroborated by |
|---|---|---|
| `0x0004` | UV_Warmwasser | `Umschaltventil` = "Richtung Warmwasser", HKP off |
| `0x0024` | + Interne_Pumpe | `0x7660[1]` = 10 %, `0x7663[1]` = 0 % (pump overrun) |
| `0x20B0` | Zubringer + Interne + UV_Heizen + HKP1 | `Umschaltventil` = "Richtung Heizen", burner off |
| `0x20B2` | + Brenner | `0xA305[1]` = `0x01`, modulation 11–15 % |

Under LSB-first, `0xB2` raises `Sammelstoerung` and leaves `Brenner` clear while
the boiler demonstrably fires.

Consequence: `HK_Frostgefahr_aktivA1M1` (BitPosition 135, i.e. byte 16 of the
22-byte block at `0x2500`) is mask `0x01`, not `0x80`. Reaching it also required
`binary_sensor` to accept a **block read at the base plus `byte_offset`** — the
same aligned-read pattern `sensor`/`text_sensor` already used, because a single-byte
read at the interior address `0x2510` is unaligned and NAKs on P300 (and returned a
constant `0xFF` on KW).

Every bitmask this generator emits from Vitosoft data is a *derivation*, not an
observation. Confirm each against hardware before trusting it.

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

**Transient-error tolerance.** `sensor` and `number` publish NAN (HA
"unavailable") only after **three consecutive failed reads**; a single CRC
glitch or timeout no longer blanks an hourly-polled entity until its next
poll, and a successful publish resets the streak. Failed **writes** never
blank state at all — the device value did not change — which is why the hub
dispatches read errors to `handle_error()` and write errors to
`handle_write_error()` (default: keep state) separately.

**No `update_interval` means *every hub tick*.** `poll_interval_ms_` defaults to
`0`, which the scheduler reads as "due on every cycle". That is the right default
for a sensor and a trap for an immutable identity string: `example/` had four of
them (`Sachnummer`, two `HWHerstellNr*`, `Beschriftung_HK1` — 79 bytes) going out
once a minute forever. `disabled_by_default: true` does not help; it is a Home
Assistant hint and the device polls regardless. Always pin an interval on
anything that cannot change.

**The next-due time must be anchored on the schedule, not on `now`.** The
scheduler advances `next_due_ms_` from the *previous scheduled* time
(`components/vitohome/poll_schedule.h`), and treats an entity due within half a
hub tick as due now. Both parts are load-bearing. `now` is `millis()` sampled
*inside* `update()`, a few milliseconds after the interval anchor that invoked it,
and that offset is not constant. Re-anchoring on it (`next_due = now + interval`)
made any entity whose interval *equals* the hub tick fire or not fire depending on
whether this tick's jitter happened to exceed the previous tick's — a coin flip.
Two 2026-07-09 hardware logs, **from the same firmware binary**, disagreed: one
dropped the entire 60 s tier on its second tick, the other never did. A host
simulation with realistic non-monotonic jitter drops 5 of 12 ticks under the old
formula and 0 of 12 under the new one (`tests/native/proof_scheduler.cpp`, which
also pins the drift-free progression, the long-stall re-anchor, and the
`millis()` wrap).

---

### 8b. Two ESPHome traps the template `water_heater` sets

Both source-confirmed against ESPHome tag `2026.6.5`, both hardware-confirmed in
the 2026-07-10 log.

**`visual:` is required, not decoration.** `WaterHeaterTraits` initialises
`min_temperature_` and `max_temperature_` to `0.0f`, `TemplateWaterHeater::traits()`
never sets them, and `WaterHeaterCall::validate_()` clamps the requested target
into `[min, max]`. Omit the `visual:` block and *every* set from the Home
Assistant card arrives as `0.00 °C`. The vitohome `number` entity's own
`min_value` caught it — `[W][number]: 'Bedien WW Solltemperatur': 0.000000 < min
10.000000` — so nothing reached the boiler, but the slider was inert.

**`optimistic: true` is required too, for a different reason.**
`TemplateWaterHeater::control()` stores the commanded target only
`if (this->optimistic_)`, and `set_trigger_` is a `Trigger<>` with no arguments —
the call object never reaches the automation. With `optimistic: false` the
`set_action` reads the *previous* target and the usual `!= number.state` guard
suppresses the write entirely. The two bugs compound: fixing only the second one
turns a silent no-op into a rejected `number.set(0)`.

**`isnan()` is the wrong "no state yet" test.** `esphome::number::Number` and
`esphome::sensor::Sensor` both declare a bare `float state;` with **no
initialiser**. Before the first successful poll it reads `0.0`, not `NAN`. A
`current_temperature`/`target_temperature` lambda guarded with `std::isnan()`
therefore publishes a spurious `0.00 °C` at boot. Use `has_state()`.

---

### 8c. There is no evidence for any P300 read-length cap

`MAX_P300_READ_LENGTH = 37` is inherited lore. Every attempt to justify it has
failed.

* The openv [Protokoll 300](https://github.com/openv/openv/wiki/Protokoll-300)
  specification describes the length byte as the count of bytes between `0x41`
  and the checksum, and names **no maximum**.
* vcontrold defines no read wider than 9 bytes — but that is
  [a documented limitation of vcontrold itself](https://github.com/openv/openv/wiki/vcontrold.xml).
* This document previously cited a 40-byte read at `0x7362` failing on P300 while
  succeeding on KW. **That was a misdiagnosis** (see §8g): `0x7362` is not a
  datapoint. A *two-byte* read at the same address fails with a byte-identical
  error telegram.

A **42-byte read succeeds on P300** (`0x7360`, 2026-07-10):

```
>>> 41:05:00:01:73:60:2A:03                (0x2A = 42 bytes)
<<< 06:41:2F:01:01:73:60:2A:<42 bytes>:2B  (length byte 0x2F = 47)
```

22 and 32 were proven the same day. `37` is therefore not merely uncited — it is
**below a read that demonstrably works**.

And there is a plausible account of where it came from. The response to a 32-byte
read opens `41:25:...` — length byte `0x25` = **37**, because the P300 length byte
counts `5 + payload`. Someone reading a telegram length byte as a data length
would land on exactly 37. Speculative, but it is the only story that fits the
number.

`MAX_P300_READ_LENGTH` is now **48**: the ceiling we are willing to *attempt*. It
covers every block the catalogs emit (widest is the 42-byte `Beschriftung_*`
block, proven) and matches `RAW_READ_MAX`, so the raw scan console can test any
block read before you enable the entity that performs it. Bytes 43..48 are
unverified. Raise it with evidence, not lore.

The console's read cap was raised from 32 to 48 for exactly that reason: it could
not test the reads the generator emits, which is why this question stayed open a
session longer than it needed to. A raw read stores no payload, so the only real
limits are the engines' packet-length arithmetic (safe well past 200) and the
dump buffer (grown 160 → 208; `RAW_DUMP_MAX_HEX` also raised to 48, or the
console elides the very bytes you asked for). Raw *writes* stay capped at 32:
they carry an inline payload.

---

### 8d. GWG addresses a single byte

Source-confirmed, and it invalidates every generated catalog under
`protocol: GWG`.

`PacketGWG::createPacket()` carries one address byte and **rejects** any
address above `0xFF` — a guard inherited verbatim from upstream `edc059a7`
(the `addr & 0xFF` serialisation below the guard is unreachable for an
over-range address). An earlier revision of this section claimed the high byte
was discarded silently; that was wrong against both the vendored copy and
upstream, and is corrected here. The rejection is memory-safe but *terminal*:
a packet the engine refuses to build never leaves the hub's dispatch lane, so
one such entity at the front of the read or write queue stalls that lane —
and everything queued behind it — permanently.

vcontrold agrees. Its GWG device — `<device ID="2053" name="GWG_VBEM"
protocol="GWG"/>` — overrides **all 26** of its addresses onto a single byte
(`0x00`, `0x01`, `0x05`, `0x22`, `0x3F`, `0x41`, `0x63`, …) rather than reusing
the 16-bit addresses the same commands use on KW/P300. GWG has its own address
space.

So the Vitosoft catalogs, which carry 16-bit addresses, **do not apply to GWG at
all**. The hub's `FINAL_VALIDATE_SCHEMA` now rejects any `address`,
`state_address` or `target_address` above `0xFF` when `protocol: GWG`. This is a
hard error, not a warning: unlike the P300 length question, the permanent
lane stall is a property of code we ship and is unconditionally wrong.

This check immediately proved that `tests/common.yaml` had been addressing
garbage under the GWG wrapper since it was written (`0x0800` → `0x00`). GWG now
has its own `tests/common-gwg.yaml`.

---

### 8e. vcontrold numbers bits LSB-first; Vitosoft numbers them MSB-first

Two conventions coexist in the ecosystem, and mixing them is almost certainly
how `1 << (bit_pos % 8)` got into the generator in the first place.

vcontrold's `<bit>N</bit>` is **LSB-first by construction**. Its `Bitstatus`
unit is defined as `icalc get="(B0 & (0x01 << BP)) >> BP"` — mask `0x01 << BP`.
Those `<bit>` numbers are hand-curated in vcontrold's own `vito.xml`.

Vitosoft's `BitPosition` is **MSB-first** (index 0 = `0x80`), hardware-proven
twice: see §8 above.

They are different numbers for different data sources. **Never copy a `<bit>`
value from vcontrold into a vitohome `bit_mask` and vice versa.** vcontrold's
`getBrennerStatus` for device 2053 uses `<bit>1</bit>`, which under its own
convention is mask `0x02` — under Vitosoft's it would be `0x40`.

---

### 8f. The export contradicts itself on 5 bit datapoints

`BitPosition` is normally the absolute bit index across the block, so
`byte = BitPosition // 8`. Where the export also gives a non-zero
`BytePosition`, the two agree for 146 of 151 informative single-bit rows. The
five that disagree:

| datapoint | BitPosition | implies byte | declares BytePosition | FCRead |
|---|---|---|---|---|
| `nviConsumerDmd_Attribute1_CFDM~0xA385` | 24 | 3 | 2 | `Virtual_READ` |
| `nvoConsumerDmd_Attribute1_LFDM~0xA346` | 24 | 3 | 2 | `Virtual_READ` |
| `OT ID0 LowByte Bit 10` | 1 | 0 | 1 | `OT_Physical_Read` |
| `OT ID0 LowByte Bit 11` | 2 | 0 | 1 | `OT_Physical_Read` |
| `OT ID0 LowByte Bit 12` | 3 | 0 | 1 | `OT_Physical_Read` |

The three `OT` rows are byte-relative and are filtered out as unreachable
anyway. The two `nvo`/`nvi` rows are `Virtual_READ` and **were reaching
catalogs**, with `byte_offset: 3` derived from `BitPosition`. We cannot tell
which field is right, so `gen_catalog.py` now emits a comment instead of an
entity that might silently read the wrong byte. 54 catalogs lost 2 entities each.

---

### 8g. Never fabricate an address from `BytePosition`

`Beschriftung_HK1~0x7360` is `BlockLength 42`, `BytePosition 2`, `ByteLength 40`.
The generator's string path computed `address = 0x7360 + 2 = 0x7362` and read 40
bytes. **`0x7362` is not a datapoint.** It is byte 2 of the block — an unaligned
interior read, the exact failure mode this component reads blocks at their base
to avoid.

Hardware, 2026-07-10, P300, via the raw scan console:

```
>>> 41:05:00:01:73:62:02:DD          (read 0x7362, TWO bytes)
<<< 06:41:06:03:01:73:62:01:01:E1    (MessageIdentifier 0x03 = Error)
```

Byte-identical to the error the 40-byte read produced. **The width was never the
problem.** On KW the same address returns `0xFF` fill, which `decode_utf16()`
turns into an empty string — so the entity published `""` and looked like an
unnamed heating circuit rather than a bug. That silence is why it survived so
long, and it is pinned in `tests/native/proof_string_offset.cpp`.

Blast radius: 72 entities across 28 catalogs, every one a `Beschriftung*`
(the only seven string datapoints in the export with `BytePosition > 0`).

Fixed on both sides:

* `text_sensor` types `ascii` and `utf16` now accept `byte_offset` +
  `byte_length`, the same aligned-block shape `sensor`, `binary_sensor` and
  `enum` already used. Without `byte_offset` the schema is unchanged.
* `gen_catalog.py` emits `address: <block base>`, `length: <block>`,
  `byte_offset`, `byte_length` — and a **comment instead of an entity** when the
  block read would exceed `MAX_TEXT_BLOCK_LENGTH`. It no longer performs address
  arithmetic anywhere.

**Confirmed the same day.** The raw scan console read the block base:

```
>>> 41:05:00:01:73:60:16:EF
<<< 06:41:1B:01:01:73:60:16:00:0B:48:00:65:00:69:00:7A:00:6B:00:72:00:65:00:69:00:73:00:20:00:DF
```

22 bytes, `MessageIdentifier 0x01`, checksum verified. Bytes 2..21 decode to
`"Heizkreis "`. The base answers; only `base + BytePosition` is rejected. Those
exact bytes are now an assertion in `proof_string_offset.cpp`.

A 32-byte read of the same block, minutes later, resolved the leading bytes:

```
<<< 06:41:25:01:01:73:60:20:00:0B:48:00:...:20:00:31:00:FF:FF:FF:FF:FF:FF:FF:FF:1C
```

Byte 1 = `0x0B` = **11**, and the field decodes to `"Heizkreis 1"` — exactly 11
characters, followed by four `0xFFFF` fill units. Byte 1 is the label's character
count. **Hardware-confirmed**, no longer inferred. Bytes 0..1 are declared by no
datapoint in the export; the block layout is:

| bytes | meaning |
|---|---|
| 0 | `0x00` |
| 1 | character count |
| 2..41 | UTF-16LE label, 20 code units, unused slots `0xFFFF` |

The general rule, now enforced in every platform, in the generator, and by a
sweep test (`test_no_emitted_address_is_ever_block_base_plus_byte_position`):
***`Address` is the block base. `BytePosition` is an offset into the response,
never into the address.*** `gen_catalog.py` contains no `addr + byte_position`
arithmetic at all.

#### The same bug on the write side

The invariant had a second violation, larger than the first. A writable field at
`BytePosition > 0` was emitted as `address: <base + BytePosition>` (the write
target) plus `state_address: <base>` (the aligned read) — **402 entities across
124 catalogs**, including `0x7661`, the address these very notes record as NAKing
on P300.

Checked against the export: of the 360 writable datapoints with
`BytePosition > 0`, `base + BytePosition` is a declared address for only **144**,
and where it *is* declared it generally belongs to an unrelated datapoint of
another device family. There is no evidence that any of them is the field's write
register.

A read of a non-existent address is a NAK. A **write** to a wrong-but-existing
address changes something. So those entities are now **demoted to read-only**:
the block-base read survives (it was always correct), the invented write target
is gone, and each carries `# NOTE: field at BytePosition N has no declared write
address -> demoted to read-only`. `COMMAND_STATE_ADDR`, which supplies real
declared state addresses, is unaffected — and no datapoint in the export combines
it with a non-zero `BytePosition`.

#### The padding is byte-oriented

The full 42-byte read also settled the block layout, and exposed a decode bug:

| bytes | meaning |
|---|---|
| 0 | `0x00` |
| 1 | character count (`0x0B` = 11) |
| 2..41 | UTF-16LE label, 20 code units |

The fill after the label is **thirteen** `0xFF` bytes, then five `0x00` — an odd
run, not code-unit aligned. Code unit 17 is therefore `0x00FF`, and
`decode_utf16()`, which *skipped* `0xFFFF` fill, walked straight into it and
published `"Heizkreis 1ÿ"`. It now **terminates** on `0xFFFF`: U+FFFF is a Unicode
noncharacter and cannot appear in a label, so a fill slot means the string is
over. Regression-pinned in `test_decode.cpp` and `proof_string_offset.cpp`.

Beware the address space is **overloaded across device families**. On heat-pump
(`WPR`) tokens, `0x7360`, `0x7361` and `0x7362` are three separate one-byte
datapoints; on boiler tokens `0x7360` is a 42-byte label block. The three label
blocks are contiguous — `0x7360`, `0x738A`, `0x73B4`, spaced exactly
`0x2A` = 42 apart — which is itself a check on the declared `BlockLength`.

One row in the export is worth knowing about: `WPR_S_Endzeit_Nachtbetrieb~0x7162`
carries `<Address>0x7362</Address>`. Its ID token and its `Address` field
disagree. The generator trusts `Address`, and that datapoint (1 byte,
`BytePosition 0`) is unrelated to the label block.

---



---

### 9a. Frame logging belongs in the component, not in `uart: debug:`

ESPHome's UART debugger has no notion of an Optolink telegram, so it has to be
told where a frame ends. The `after: delimiter: [0x06]` recipe that circulates
for Optolink is the **P300 ACK byte**. On KW, `0x06` is an ordinary data byte,
so the debugger splits telegrams mid-frame — visible in the 2026-07-09 logs as
`>>> 01:F4:23:06` followed by `>>> 01:24` for a single write to `0x2306`.

The adapter can reconstruct the boundaries and needs no delimiter.
`log_frames: true` on the hub sets `-DVITOHOME_LOG_FRAMES`, and
`vito_uart_interface.h` then logs one line per telegram under the
`vitohome.frames` tag. **Both directions are buffered**, and each is flushed on
the first of: traffic in the opposite direction, an inter-byte gap over 30 ms, or
a full buffer.

Buffering TX is not optional, though the first cut assumed otherwise ("one
`write()` == one telegram, on every protocol"). That holds for VS1/KW and GWG and
is **false for VS2/P300**: `_sendStart()`, `_sendPacket()` and `_sendCRC()` are
three states of the VS2 send machine, each issuing its own `write()` one `loop()`
apart. A single request reached the log as three lines.

The 30 ms constant comes off the wire, not from taste. At 4800 8E2 a byte occupies
11 bits = 2.29 ms. Measured on P300: the gap between the three TX pieces of one
telegram is 18–20 ms, while the gap between the master's ACK (`0x06`) and the next
telegram's `PACKETSTART` is ~35 ms. 30 ms therefore joins a telegram and separates
the ACK from it, and is still far below KW's ~2.2 s idle-sync cadence.

It is a build flag rather than a runtime setter so a production firmware carries
no buffers and no per-byte branch; `frame_tick()` degrades to an empty inline.

One consequence worth knowing: a frame is logged when it *closes*, so the `<<<`
line lands after the decoded value it produced. The contents are right; only the
interleaving reads oddly.

---

## 10. Testing and the gate model

The gates are **sequential and non-substituting** — passing an earlier gate does
not vouch for a later one:

```
host C++ : decode/encode tests           tests/native/test_decode.cpp   (380 checks)
host C++ : VS2 transaction harness        tests/native/test_vs2_transaction.cpp (8/8)
host C++ : engine / GWG compile-proofs    engine_compile_proof.cpp, proof_gwg_poke.cpp
host C++ : VS2 parser zero-payload (OOB)  tests/native/proof_vs2_zero_payload.cpp  (ASan/UBSan)
python   : validators + catalog generator tests/unit/  (pytest)
lint     : ruff check / ruff format
format   : clang-format  (pinned v22.1.5)
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
  tester. KW/VS1 and P300 are hardware-confirmed. Note the vendored engine now
  deliberately diverges from upstream here: upstream's completion check waited
  for the request-frame length (5 bytes) instead of the datapoint length, so no
  GWG read of length != 5 could ever have completed — fixed and host-proven
  (`proof_gwg_read.cpp`, THIRD_PARTY.md #8; the read side is source-confirmed
  against vcontrold's GWG framing, the write-ack side is model-derived).
  The KW sibling had the same completion bug on **writes**: upstream waited
  for `datapoint.length()` bytes where the device acks with a single `0x00`
  -- hardware-confirmed by a live VScotHO1_72 capture in which the 8-byte
  clock write to `0x088E` was acked but reported as a timeout. Fixed and
  host-proven (`proof_vs1_write.cpp`, THIRD_PARTY.md #11); 1-byte writes were
  never affected, which is why Betriebsart writes always worked.
- **`Convert4BytesToFloat`** (IEEE-754 datapoints) is not yet a converter; such
  datapoints are surfaced by the catalog generator as commented hints rather than
  decoded wrongly. Note this is *not* the same as `sec2hour`, which reads 4 bytes
  as a `uint32`. (`RotateBytes` and `HexByte2UTF16Byte` were previously in this
  commented-hint set and are now handled — the big-endian `rotatebytes` converter
  and the `type: utf16` text_sensor, both host-tested in `test_decode.cpp`; the
  `rotatebytes` preset's registry shape and length rule are additionally pinned
  in `tests/unit/test_validators.py`.)
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
