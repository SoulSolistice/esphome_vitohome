# Converters and decode reference

This is the reference for authoring a datapoint by hand, and for understanding
a generated one. It covers how each platform turns a raw Optolink payload into
an entity state.

## Model

Optolink payloads are raw little-endian bytes. vitohome decodes and encodes
them itself rather than relying on the transport engine. A `converter:` names a
preset that controls how a numeric payload is scaled:

- the raw bytes are read into a 64-bit integer,
- scaled in `double` precision,
- and narrowed to the float that ESPHome's sensor state requires only at the
  final step.

Working in 64-bit integer and `double` keeps four-byte counters correct: the
raw seconds value of a 4-byte counter exceeds the range of integers a 32-bit
float can represent exactly, so scaling in `double` avoids losing low-order
bits before the value is reduced to something small (hours, degrees, percent).

Writes are the exact inverse: the value is divided by the scale, rounded to the
nearest raw step, and range-checked against the byte width before anything is
transmitted.

## Numeric converters

These back the `sensor` platform, and the encodable subset backs the writable
`number` platform.

| `converter` | scale | default sign | valid `length` | writable (`number`) |
|---|---|---|---|---|
| `noconv` | x1 | unsigned | 1, 2, 3, 4 | yes |
| `div2` | x0.5 | **signed** | 1, 2 | yes |
| `div10` | x0.1 | **signed** | 1, 2 | yes |
| `div100` | x0.01 | unsigned | 1, 2, 4 | yes |
| `div1000` | x0.001 | unsigned | 2, 4 | yes |
| `sec2hour` | / 3600 | unsigned | 4 | no (read-only) |
| `mult2` | x2 | unsigned | 1, 2, 4 | yes |
| `mult5` | x5 | unsigned | 1, 2, 4 | yes |
| `mult10` | x10 | unsigned | 1, 2, 4 | yes |
| `mult100` | x100 | unsigned | 1, 2, 4 | yes |
| `rotatebytes` | x1 (**big-endian**) | unsigned | 2 | no (read-only) |

**Sign.** `div2` and `div10` decode the raw integer as signed (two's
complement), so a sub-zero temperature reads correctly; every other converter
is unsigned by default. An explicit `signed:` on the entity overrides the
default for that datapoint.

**Encodability.** A writable `number` accepts only converters that have a
defined inverse; `sec2hour` is read-only and cannot back a `number`.

**Byte order.** Payloads are little-endian by default. `rotatebytes` (Vitosoft `RotateBytes`) is the exception: the same bytes are assembled **big-endian** before scaling (`decode.h::read_be` / `decode_scaled_be`). It is read-only and currently used for 2-byte coding values (`GWG_Codierstecker_Kennziffer`).

## Decode by platform

A `converter:` is one of several ways a value is read. The full set:

| platform | how the value is read | key options |
|---|---|---|
| `sensor` | scaled integer | `converter`, `length`, `signed` |
| `number` (write) | scaled integer, encodable converters only | `converter`, `length`, `min_value` / `max_value` / `step` |
| `binary_sensor` | one bit of a byte | `bit_mask`, `length` |
| `text_sensor` `type: ascii` | raw bytes as an ASCII string | `length` |
| `text_sensor` `type: utf16` | raw bytes as a UTF-16LE string | `length` (even) |
| `text_sensor` `type: error_history` | 9-byte slot: code byte + 8-byte BCD timestamp, mapped via `codes:` | `length`, `codes` |
| `text_sensor` `type: enum` | raw value mapped to a label | `options` |
| `text_sensor` `type: device_id` | the device identification string | (none) |
| `text` | 8-byte-per-day switching-time program, read/write as a canonical `"HH:MM-HH:MM ..."` string | `address`, `read_back` |
| `select` | raw value mapped to a label, writable | `options`, `address`, `state_address` |
| `switch` | boolean register, writable | `on_value` / `off_value`, `on_values`, `address`, `state_address` |
| `binary_sensor` `type: connectivity` | hub-fed Optolink link state, no address | (none) |
| `event` | fault-code slot; fires an HA event on code change | `address`, `length`, `codes` |

**ASCII** (`type: ascii`): each raw byte is one character. A NUL byte
terminates the string, trailing spaces are trimmed, and any non-printable byte
becomes `?`, so a bad read yields a safe string rather than control characters.
Used for part and serial numbers.

**UTF-16** (`type: utf16`): the payload is a UTF-16LE byte-string decoded to UTF-8 (Vitosoft `HexByte2UTF16Byte`). `length` is the field width in **bytes** and must be even; a `0x0000` unit terminates, `0xFFFF` fill is skipped, and trailing spaces are trimmed. Used for the editable heating-circuit labels (`Beschriftung_HK1..3`), emitted read-only (display); round-trip editing would need a `text:` platform with a UTF-16 encode path.

**Bit** (`binary_sensor`): `bit_mask` selects which bit of the addressed byte
drives the state; `length` sizes the read.

**Enum** (`type: enum` and `select`): `options` maps raw wire values to labels.
A wire value not present in `options` is logged and left unpublished rather than
shown as a wrong label.

**Two-address `select`.** Some Viessmann controls accept a command at one
register but expose the resulting state at another. A `select` handles this with
two addresses:

- `address:` is the **command (write)** address,
- `state_address:` (optional) is the **read (state)** address.

Polling, read-back and read-response matching use the state address; the write
goes to the command address. For example, party mode is commanded at `0x2330`
(`NRx Partybetrieb`) but its live state is read at `0x2303`
(`BedienPartybetrieb`). Omit `state_address:` for a single-address select, which
reads and writes the one `address:`.

This read/write-address split is available on `select` only. A writable `number`
reads and writes a single `address`; a numeric datapoint whose live state is
exposed at a different register than its command cannot be expressed today, and
would need `number` to gain the same `state_address`.

**Schaltzeiten (`text`).** Each entity is one weekday's switching-time program
-- a fixed 8-byte block (four ON/OFF switch-point pairs) at `address`, read and
written at the same register so the hub's read-back re-reads exactly what was
sent. The wire bytes encode/decode to a canonical
`"HH:MM-HH:MM HH:MM-HH:MM ..."` string (up to four pairs, space-separated,
`"--"` or a blank pair leaves that slot disabled); an empty string clears the
whole day (all bytes `0xFF`). A malformed input string is rejected in full --
partial writes never reach the device. Minutes are truncated to the device's
10-minute grid on write, not rounded to the nearest step. `read_back` (default
`true`, optional `update_interval`) re-reads immediately after the write ACK,
so the Home Assistant state snaps to the accepted grid value instead of
sitting on the raw input for one poll cycle. There is currently no
`state_address`/`address` split for `text` -- read and write always share the
one configured `address`.

**Validation scope: a pure binary packer.** The encoder validates only what
the wire format can represent -- hour 0..23, minute 0..59, at most four pairs,
per-token grammar -- and deliberately does **not** validate scheduling
semantics: ascending pair order, non-overlap, and ON-before-OFF within a pair
are unchecked. This matches the independent reference: vcontrold's
`setCycleTime` (`src/unit.c`) performs no ordering validation either (it does
not even range-check the hour). Whether the device itself requires ordered or
contiguous pairs is unverified on hardware; scheduling logic and sanity checks
belong in Home Assistant or with the user, not in this codec.

**Switch** (`switch`): a two-state specialisation of `select` for semantic
on/off registers (Partybetrieb `0x2330`, the K-coding booleans), so Home
Assistant gets a native toggle -- `switch.turn_on/off`, voice assistants,
binary automation conditions -- instead of a two-option dropdown. `on_value` /
`off_value` are the raw wire values written for on/off (default `1`/`0`;
configurable because e.g. coding address `K8A` uses `175`=aktiv /
`176`=inaktiv). `on_values` optionally replaces the set of wire values that
*read back* as ON, for registers that report extra on-ish states; anything
that is neither an `on_values` entry nor `off_value` keeps the last state and
is logged, the same policy `select` applies to unmapped values. The
`state_address` read/write split and `read_back` behave exactly as on
`select`. Two options alone do not make a boolean -- Celsius/Fahrenheit or
Einkessel/Mehrkessel are choices and stay selects (also the rule
`gen_catalog.py`'s emission heuristic follows). `restore_mode` is pinned to
`DISABLED` and other values are rejected at config time: state always comes
from the device, and a boot-time restore would write to the heater on every
reboot.

**Connectivity** (`binary_sensor` `type: connectivity`): hub-fed, no address,
never polls -- the hub publishes its own view of the Optolink link
(`device_class: connectivity`, `entity_category: diagnostic` by default).
ONLINE on any successful response; OFFLINE when the start-up protocol
verification fails or after three consecutive protocol errors (watchdog
expiries count). Edge-published, so a healthy bus produces no state traffic.

**Fault events** (`event`): polls a fault-history slot (typically FA01, the
newest fault -- `0x7507` on the B3HA; slot layout is a code byte plus an
8-byte BCD timestamp) and fires a Home Assistant event when the code
*changes*: a new fault fires its hex code (`0x10`), a cleared slot fires
`cleared`, and a code outside `codes:` fires `unknown` with the raw value in
the log. The `codes:` map has the same shape as `text_sensor`
`type: error_history` and defines the event-type space HA sees. The first
successful poll only records a baseline and never fires, so the fault sitting
in the slot at boot does not spam the logbook on every reboot; a read error
keeps the baseline (a bus glitch must not manufacture fault events). This
complements the `error_history` text sensor, which shows slot contents but
cannot notify.

**DHW as `water_heater`** -- see `example/vitohome-dhw.yaml`: ESPHome's
`water_heater` `template` platform wraps the existing datapoints (tank
temperature `0x0804`, writable setpoint `0x6300`, effective setpoint `0x6500`)
into a native HA water-heater card, no C++ and no custom HA integration. DHW
on/off stays with the shared Betriebsart register by design.

## Validation

Length and range are checked at `esphome config` time, before a wrong value can
reach the device:

- The configured `length` must be one of the converter's valid lengths
  (the table above); a mismatch is rejected during validation.
- For a writable `number`, the encodable range derived from the converter and
  `length` is checked against `min_value` / `max_value`, using the same range
  check the C++ encode path applies at runtime — so the config-time check and
  the on-device guard agree.

## Conversions that are not modelled

Some Vitosoft conversions still cannot be represented (for example true
floating-point conversions, `Convert4BytesToFloat`). The catalog generator
emits these as commented hints in the generated package rather than decoding
them incorrectly, so you can decide how to handle them by hand.
