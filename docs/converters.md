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

**Sign.** `div2` and `div10` decode the raw integer as signed (two's
complement), so a sub-zero temperature reads correctly; every other converter
is unsigned by default. An explicit `signed:` on the entity overrides the
default for that datapoint.

**Encodability.** A writable `number` accepts only converters that have a
defined inverse; `sec2hour` is read-only and cannot back a `number`.

## Decode by platform

A `converter:` is one of several ways a value is read. The full set:

| platform | how the value is read | key options |
|---|---|---|
| `sensor` | scaled integer | `converter`, `length`, `signed` |
| `number` (write) | scaled integer, encodable converters only | `converter`, `length`, `min_value` / `max_value` / `step` |
| `binary_sensor` | one bit of a byte | `bit_mask`, `length` |
| `text_sensor` `type: ascii` | raw bytes as an ASCII string | `length` |
| `text_sensor` `type: enum` | raw value mapped to a label | `options` |
| `text_sensor` `type: device_id` | the device identification string | (none) |
| `select` | raw value mapped to a label, writable | `options`, `address`, `state_address` |

**ASCII** (`type: ascii`): each raw byte is one character. A NUL byte
terminates the string, trailing spaces are trimmed, and any non-printable byte
becomes `?`, so a bad read yields a safe string rather than control characters.
Used for part and serial numbers.

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

Some Vitosoft conversions cannot be represented as a scale (for example true
floating-point conversions). The catalog generator emits these as commented
hints in the generated package rather than decoding them incorrectly, so you
can decide how to handle them by hand.
