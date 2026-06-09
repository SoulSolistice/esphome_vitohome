# vitohome — Stage 2 Design

This document explains the decisions behind the Stage 2 implementation: why the
component decodes Optolink payloads itself instead of leaning on VitoWiFi's
converters, the precision problem that drove that choice, the corrected
error-history layout, the device-identification flow, the write path, and the
catalogue tooling. Protocol facts here were verified against VitoWiFi at the
pinned commit `edc059a7` and ESPHome at tag `2026.5.0`; per §0 of the project
guidelines, anything version-sensitive is stated as verified-against-source,
not from memory.

---

## 1. Architecture in one paragraph

VitoWiFi is used purely as the P300 (VS2) wire engine: framing, sequencing,
ACK/NAK, CRC, retries. Every datapoint is constructed with `VitoWiFi::noconv`
and writes go through the raw-bytes overload
`write(const Datapoint&, const uint8_t*, uint8_t)`. The component does all
value decoding and encoding itself in `decode.h`. The Python layer turns YAML
into three things the C++ runtime needs per entity: a `VitoWiFi::Datapoint`
(name, address, length, `noconv`), a decode scale, and a signedness flag. This
keeps a single, testable decode path (host-compiled, no hardware) and sidesteps
two real hazards in VitoWiFi's converter layer described below.

```
YAML ──Python codegen──▶ Datapoint(noconv) + scale + signed
                              │
   bus bytes ◀── VitoWiFi (frame/CRC/seq) ──▶ raw payload
                              │
                       decode.h (double) ──▶ float32 state
```

---

## 2. Why not use VitoWiFi's converters

### 2.1 The non-discriminated `VariantValue` union

VitoWiFi returns decoded values through a `VariantValue` that is a **tagless**
union. Nothing in the type records which member was written, so reading the
wrong member returns whatever bit pattern happens to be there — a silent
garbage value, not an error. Picking the correct member requires knowing the
converter that produced it, i.e. exactly the information the union throws away.
Doing the decode ourselves removes the guessing: we read the bytes and apply a
known scale.

### 2.2 float32 math loses bits on 4-byte counters

VitoWiFi performs converter arithmetic in `float`. A 32-bit counter can exceed
float32's exact-integer ceiling of `2**24 = 16,777,216`, after which not every
integer is representable. Burner-hours is the concrete case on the reference
unit.

---

## 3. Precision analysis (worked example)

The burner-hours datapoint (`0x08A7`, Sec2Hour) is a 4-byte little-endian
seconds counter. Take a real-scale value of **212,197,680 s**:

| step | float32 path (old) | double path (vitohome) |
|------|--------------------|------------------------|
| raw read | `212197680` is **> 2²⁴**, so the nearest float32 is `212197680` only if it lands on a representable step — in general it is rounded | exact in `uint64_t` |
| ÷ 3600 | accumulated rounding | `58943.8` h |

`decode.h` reads the raw integer with `read_le()` into a `uint64_t`
(bit-exact), scales in `double`, and **narrows to float32 only at the very
end** when handing the value to ESPHome's `publish_state(float)`. The
host test `tests/native/test_decode.cpp` asserts both halves of this:

```cpp
const uint8_t le[] = {0x30, 0xE1, 0xA5, 0x0C};   // 212,197,680 little-endian
CHECK(read_le(le, 4) == 212197680u);             // bit-exact integer read
double v;
decode_scaled(le, 4, 4, /*signed*/ false, 1.0/3600.0, &v);
CHECK(close_to(v, 58943.8, 0.05));               // correct hours
```

The float32 state ESPHome ultimately stores still can't represent every large
integer exactly, but for a *scaled* quantity like hours (≈59 000.0) that is
fine; the bug being fixed is the loss that happened **before** scaling, while
the value was still a large raw integer.

The same double-then-narrow discipline is why temperatures are decoded
correctly when below zero (see §4).

---

## 4. The converter model

A `converter:` in YAML is a **preset**, not a call into VitoWiFi. Each maps to
`(scale, default_signed, lengths, encodable)`:

| converter | scale | signed by default | lengths | encodable |
|-----------|-------|-------------------|---------|-----------|
| `noconv`  | 1     | no  | 1,2,3,4 | yes |
| `div2`    | 0.5   | **yes** | 1,2 | yes |
| `div10`   | 0.1   | **yes** | 1,2 | yes |
| `div100`  | 0.01  | no  | 1,2,4 | yes |
| `div1000` | 0.001 | no  | 2,4 | yes |
| `sec2hour`| 1/3600| no  | 4   | **no** |
| `mult2/5/10/100` | 2/5/10/100 | no | 1,2,4 | yes |

Two points worth calling out:

* **Signedness.** The Viessmann `Div2`/`Div10` conversions are signed. An
  outside temperature of −4.0 °C arrives as raw `0xFFD8` (= −40). The old
  config used a length-2 read with `multiply: 0.1` and **no sign handling**,
  which would have published `6549.6 °C`. `div10` is signed, so `decode.h`
  sign-extends first (`sign_extend_le(0xFFD8, 2) == -40`) and scales to −4.0.
  An explicit `signed:` in YAML overrides the default (used for the Niveau
  coding value, a signed integer under `noconv`).

* **`lengths` semantics changed from Stage 1.** In Stage 1 the component used
  VitoWiFi's converters, so the allowed lengths mirrored VitoWiFi's internal
  asserts. Now that decoding is in-component, the length sets are about what is
  *physically sensible and float32-safe after scaling*. The check that remains
  load-bearing is the per-`number` encodable-range cross-check (§6), not the
  length table.

`sec2hour` is deliberately **not encodable**: nobody writes an hours counter,
and its scale has no exact inverse for arbitrary float inputs.

---

## 5. Error-history layout (corrected)

The old `esphome_vitoconnect` config read 3 bytes at `0x7507` and treated them
as `[code, day, month]`. That layout was a guess and is **wrong**.

The authoritative layout comes from the reverse-engineering repo's own decoder
(InsideViessmannVitosoft, `Viessmann2MQTT.py`, `DateTimeFromBCD`): a slot is
**9 bytes**, `[0]` = error code followed by an 8-byte packed-BCD timestamp:

| byte | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|------|---|---|---|---|---|---|---|---|---|
| field | code | year-hi | year-lo | month | day | weekday* | hour | minute | second |

\* weekday is `0 = Monday` and is ignored.

`decode.h::decode_datetime_bcd()` validates every nibble is BCD (so the
`0xFF`-filled empty slots fail cleanly), range-checks the fields, and rejects
implausible years (`< 1990`) so an all-zero slot doesn't decode as year 0.
The `error_history` text sensor emits `"<label> (0x<code>) @ YYYY-MM-DD
HH:MM:SS"`, falling back to code-only when the timestamp is empty/invalid.

> **Hardware caveat.** On the reference unit, the old config could only read the
> single code byte; per-byte reads at `0x7508+` returned a P300 error. A **9-byte
> block read at `0x7507` is a different transaction** and is the right Stage-2
> approach (it is how Viessmann2MQTT reads it), but it must be confirmed on the
> actual unit. If the device NAKs the block read, drop `length:` to 1 for a
> code-only sensor.

---

## 6. The write path

`number` and `select` stage a raw payload (`encode_scaled` / a little-endian
enum value) into the entity's buffer and call `request_write(this)` on the hub.
The hub keeps two deques and **writes preempt reads** so a user setpoint change
doesn't wait behind a full poll cycle. On the device ACK, if `read_back` is
true (default), a read of the same address is pushed to the **front** of the
read queue so Home Assistant reflects the device's own view rather than an
optimistic guess; with `read_back: false` the entity publishes optimistically
on ACK.

The encodable-range check in `number.py` mirrors `encode_scaled` **exactly**
(round to nearest raw step, then range-check for byte width and sign) and runs
at `esphome config` time:

```
raw = round(value / scale)
fits = raw_fits(raw, length, signed)   # same bounds as decode.h::encode_scaled
```

This matters because VitoWiFi's own runtime guards are compiled out under
`NDEBUG`, and because catching an un-encodable bound at config time is far
better than a runtime "value not written" log. `raw_fits` is unit-tested
against signed/unsigned boundaries for 1/2/4-byte widths.

`select` uses ESPHome 2026.5.0's preferred index-only `control(size_t)`
override (the string variant forwards to it; `Select::state` is deprecated and
removed in 2026.7.0). Options are an **ordered `{raw_value: label}` map**, so
the YAML author controls option order; the raw values are validated to fit the
byte width and the labels to be unique.

---

## 7. Device identification

With `identify_device: true` (default), the hub runs a small state machine
after `begin()` and before regular polling: a single length-4 read at `0x00F8`,
falling back to four length-1 reads at `0xF8/F9/FA/FB` if the block read comes
up short. It is **fail-soft** — each field that can't be read is left unknown —
and the result (`group`, `controller`, `hw`, `sw`) is logged at INFO and pushed
to any `device_id` text sensors. A small built-in table names the families this
project has seen on the wire (`0x20CB` → VScotHO1, `0x2098`, `0x2094`,
`0x2053`); everything else is reported as raw hex. The authoritative
family→datapoint matching is the job of the catalogue tooling, not this table.

If the software index (`0xFB`) is unavailable, the log says so and advises
matching the Vitosoft data on the family only.

---

## 8. Per-entity polling

The hub has a base `update_interval` (the poll tick). Each entity may set its
own `update_interval`, which is honoured at **hub-tick granularity** — so an
entity interval shorter than the hub interval silently degrades to the hub
interval. Rather than let that surprise anyone, `setup()` logs a warning naming
each such entity. (There is intentionally no `final_validate` cross-check
between entity and hub intervals: the hub id isn't reliably resolvable from a
platform's `final_validate` without brittle global lookups, so the runtime
warning is the chosen tradeoff.) Suggested tiers: live measurements ~60 s,
monotonic counters ~600 s, writable coding values ~3600 s, error history
~300 s.

---

## 9. Catalogue workflow (`scripts/gen_catalog.py`)

Per-device datapoint definitions are **not** discoverable over the bus — they
live in Viessmann's Vitosoft data, surfaced by the InsideViessmannVitosoft
scripts as a `DPDefinitions.xml` .NET DataSet diffgram. `gen_catalog.py`
(stdlib-only) parses that export and emits a ready-to-include ESPHome
**package**:

```
python3 scripts/gen_catalog.py --data <export-dir> --list-devices
python3 scripts/gen_catalog.py --data <export-dir> \
    --device VScotHO1_72 --profile standard --out my-heater.vitohome.yaml
```

It joins `ecnDatapointType` → `ecnDataPointTypeEventTypeLink` → `ecnEventType`
→ `ecnEventTypeEventValueTypeLink` → `ecnEventValueType`, then routes each
datapoint to a platform:

* read-only numeric → `sensor` (counters get `total_increasing` + slow poll)
* read-only bit field → `binary_sensor` (mask from `BitPosition`)
* read-only enum → `text_sensor type: enum`
* writable enum (1 byte) → `select`
* writable numeric → `number` (borders → `min/max/step`; absent borders → a
  `0/0/1` placeholder that is valid config but pinned until you fill it in)
* non-numeric conversions (`DateTimeBCD`, `Time53`, floats, …) → emitted as
  **commented hints**, never a wrong sensor

`--profile minimal|standard|full` controls breadth; `--include`/`--exclude`
take regexes on the datapoint name. Every emitted entity is
`disabled_by_default: true`, so you opt in from Home Assistant. The generator
gets you a correct *starting point* — always `esphome config` then
`esphome compile`/`run`, because not every address answers on every firmware.

---

## 10. Validation strategy

| layer | what runs it | what it proves |
|-------|--------------|----------------|
| `tests/native/test_decode.cpp` | host `g++ -std=c++17` | the pure decode/encode logic incl. the precision fix, BCD, datetime, sign-extend, encode round-trips |
| `tests/unit/test_validators.py` | `pytest` (esphome venv) | converter registry, `raw_fits`, signed resolution, converter/length + encodable-range rejection, C++ literal escaping |
| `tests/unit/test_gen_catalog.py` | `pytest` | catalogue parser, platform routing, profiles, filters, YAML emission |
| `esphome config` | CI + manual | every platform schema and validator on a full device YAML |
| `esphome compile` (arduino + idf) | CI + manual | codegen actually compiles against VitoWiFi under both frameworks |

Two limits to keep in mind:

* `esphome config` validates schemas and runs the config-time validators but
  does **not** run `to_code`/codegen. A full `esphome compile` is the gate for
  codegen correctness.
* No amount of source inspection or host testing substitutes for a live
  `esphome run` against the real heater for **address** correctness — the
  catalogue and the reference YAML get the *shape* right; the wire confirms the
  *addresses*.

---

## 11. Known limitations / forward work

* **KW (VS1) and GWG** protocols are not implemented; Stage 2 is P300 (VS2)
  only. Those have different framing and callback shapes.
* **`Convert4BytesToFloat`** (IEEE-754 datapoints) is not yet a converter; such
  datapoints are surfaced by the catalogue as commented hints.
* The error-history **block read** at `0x7507` needs hardware confirmation on
  units where per-byte reads NAK (see §5).
* `CODEOWNERS` and the `CODEOWNERS = ["@yourhandle"]` line in
  `components/vitohome/__init__.py` still carry a placeholder handle.
