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
default for that datapoint. `scripts/gen_catalog.py` emits `signed: false` on a
`div2`/`div10` datapoint that Vitosoft declares unsigned *and* borders at 0 or
above -- a 1-byte boiler temperature of 0..127.5 degC would otherwise read
-58 degC at 70 degC (design notes §8h).

**Encodability.** A writable `number` accepts only converters that have a
defined inverse; `sec2hour` is read-only and cannot back a `number`.

**Byte order.** Payloads are little-endian by default. `rotatebytes` (Vitosoft `RotateBytes`) is the exception: the same bytes are assembled **big-endian** before scaling (`decode.h::read_be` / `decode_scaled_be`). It is read-only and currently used for 2-byte coding values (`GWG_Codierstecker_Kennziffer`).

## Decode by platform

A `converter:` is one of several ways a value is read. The full set:

| platform | how the value is read | key options |
|---|---|---|
| `sensor` | scaled integer | `converter`, `length`, `signed`, `byte_offset`, `byte_length`, `bit_mask` |
| `number` (write) | scaled integer, encodable converters only | `converter`, `length`, `min_value` / `max_value` / `step`, `state_address`, `byte_offset`, `byte_length`, `bit_mask` |
| `binary_sensor` | any bit of `bit_mask` set | `bit_mask`, `length`, `byte_offset` |
| `text_sensor` `type: ascii` | raw bytes as an ASCII string | `length` |
| `text_sensor` `type: utf16` | raw bytes as a UTF-16LE string | `length` (even) |
| `text_sensor` `type: error_history` | 9-byte slot: code byte + 8-byte BCD timestamp, mapped via `codes:` | `length`, `codes` |
| `text_sensor` `type: enum` | raw value mapped to a label | `options`, `byte_offset`, `byte_length`, `bit_mask` |
| `text_sensor` `type: device_id` | the device identification string | (none) |
| `text` | 8-byte-per-day switching-time program, read/write as a canonical `"HH:MM-HH:MM ..."` string | `address`, `read_back` |
| `select` | raw value mapped to a label, writable | `options`, `address`, `state_address`, `byte_offset`, `byte_length`, `bit_mask` |
| `switch` | boolean register, writable | `on_value` / `off_value`, `on_values`, `address`, `state_address`, `byte_offset`, `byte_length`, `bit_mask` |
| `binary_sensor` `type: connectivity` | hub-fed Optolink link state, no address | (none) |
| `event` | fault-code slot; fires an HA event on code change | `address`, `length`, `codes` |
| `button` | force-refresh: marks all datapoints due | (none) |

### Aligned field extraction (`byte_offset` / `byte_length`)

Many datapoints are a small field carved from a larger firmware block. The
KW protocol is byte-oriented and tolerates reading such a field directly at
`block_base + offset`, but **P300/VS2 rejects an unaligned interior read** and
returns an error frame (hardware-confirmed on the pump-speed bytes at
`0x7661`/`0x7664`). To stay portable, a `sensor` can read the whole block and
extract the field:

- `length` is then the **block read** (bytes fetched from the wire, at the
  block base), not the field width.
- `byte_offset` selects the field's start byte within that block.
- `byte_length` (default 1) is the field width in bytes (1–4); the converter
  is checked against this width, not the block length.

So a 2-byte `div10` room-setpoint at offset 12 of a 22-byte block is:

```yaml
- platform: vitohome
  name: "Room setpoint"
  address: 0x2500      # block base — aligned, so P300 accepts it
  length: 22           # read the whole block
  byte_offset: 12      # field starts at byte 12
  byte_length: 2       # 2-byte field
  converter: div10
```

The block read must stay within `MAX_P300_READ_LENGTH` (48 bytes -- the
attempt ceiling; a 42-byte read is hardware-proven, see the constant's note in
`components/vitohome/__init__.py`); a field in a larger block, or wider than
4 bytes, can't be a scalar extract.
`scripts/gen_catalog.py` emits this form automatically for interior fields.

**Writable fields inside a block (block mode).** A writable `select`, `switch`
or `number` with `byte_offset` and **no** `state_address` is in block mode:
`address` is the block base, `length` the whole block (at most
`MAX_RMW_BLOCK`, 16 bytes), and the field the `byte_length` (default 1; max 2
for select/switch, 4 for number) bytes at `byte_offset`. A write is a
**whole-block read-modify-write**: the hub reads the block, replaces the
field, and writes the whole block back, so every other byte goes back exactly
as it was read.

```yaml
number:
  - platform: vitohome
    name: "Neptun ZP Drehzahl"
    address: 0x7953      # block base: read AND written here
    length: 2            # the whole block
    byte_offset: 1       # the field: byte 1
    converter: noconv
    min_value: 0
    max_value: 100
    step: 1
```

Why the whole block: the device writes its **whole datapoint**. A write
narrower than the block is not a write of "just those bytes" -- the device
fills the rest of the block from whatever follows the payload in the telegram.
Hardware, VScotHO1_72 (P300): `0x7953` read `00 00`; after a 1-byte write it
held `01 D6` -- byte 1 had become the checksum byte of our own telegram. The
ACK and a field-only read-back both looked fine. Writing at the field's own
interior address is no way out either: `block_base + offset` is not a declared
write address (`0x7952` is not one; `0x7362` does not exist at all, design
notes §8g).

`byte_offset: 0` is meaningful here: it is what selects block mode for a field
at the start of a wider block. Without it, `length: 2` would be a 2-byte field.

Hardware-confirmed on VScotHO1_72 (P300): the Neptun output bit and the pump
speed byte of `0x7951`, each written while the other held a non-zero value
(`design_notes.md` §5b). `dump_config` names the mode:

```
Write: whole 2-byte block, read-modify-write of 1 byte(s) at offset 1
```

A writable entity at a block base **without** that line writes only its own
`length` bytes; check it before toggling anything you wrote by hand.

The rules from bit fields apply: nothing else is sent between the read and the
write; if the read fails, **nothing is written**; the read-back after the write
checks the whole block and warns if a byte outside the field changed. After the
write, every other polled entity on the same block is re-read too, so they show
the post-write block. Option / on-off values, the converter, and `min_value` /
`max_value` all run against the field (`byte_length`), not the block. Details:
`docs/design_notes.md` §5b.

A block that also holds **live** (read-only) data should not be written this
way: the hub writes back the value it read, which the device may have changed
in the meantime. `scripts/gen_catalog.py` therefore emits block mode only for
blocks whose datapoints are all writable settings, and keeps the rest
read-only with a note.

**Interior shape (deprecated, warns).** With `byte_offset` **and**
`state_address`, the block is read at `state_address` and the field written to
its own interior register at `address`. This was the original form; it still
validates, but `esphome config` warns: the interior address need not exist,
where one is declared it is generally another datapoint, and the write is
narrower than the datapoint. Move to block mode -- drop `state_address` and set
`address` to the block base:

```yaml
# before (warns)                     # after (block mode)
address: 0x7952                      address: 0x7951
state_address: 0x7951                length: 2
length: 2                            byte_offset: 1
byte_offset: 1
```

The read-only `text_sensor` `type: enum` supports the same extraction with no
write side at all: `address` is simply the block base, `length` the block
read, and the enum field the `byte_length` bytes at `byte_offset` matched
against `options`.

### Bit fields (`bit_mask`)

Many Viessmann coding values are not whole bytes but a few bits of one: GWG
EEPROM `0x0005` holds eight independent coding flags, `0x001E` holds two 4-bit
switching points. `bit_mask` names such a field by its **mask** -- `0xF0` is the
high nibble, `0x08` one flag -- on `sensor`, `text_sensor` `type: enum`,
`number`, `select` and `switch`.

```yaml
select:
  - platform: vitohome
    name: "K13 Speichernachlauf"
    address: 0x0005
    access: eeprom
    length: 1
    bit_mask: 0x08          # one flag of the eight in this byte
    options:
      0: "mit Nachlauf bis 10 Min."   # FIELD values, not byte values
      1: "ohne Nachlauf"

number:
  - platform: vitohome
    name: "D-Anteil Regelung"
    address: 0x0022
    access: eeprom
    length: 1
    bit_mask: 0xF0          # high nibble
    min_value: 0
    max_value: 15
    step: 1
```

**The field value.** The entity reads its byte as usual (with `byte_offset`
inside a block read, if given), then works on
`field = (byte & bit_mask) >> shift`, where `shift` is the mask's lowest set
bit. Everything downstream uses the field: option values, `on_value` /
`off_value` / `on_values`, the converter scale, `min_value` / `max_value`.

**Writes are read-modify-write.** A plain write replaces the whole byte, which
for a bit field would overwrite every other datapoint sharing it. With
`bit_mask`, the hub instead reads the byte, replaces only the masked bits, and
writes it back, so the other bits go back exactly as they were read. A bit
field in a byte of a wider block is written in block mode (`byte_offset`
without `state_address`, see above): the whole block is read, the masked bits
of that byte replaced, and the whole block written back.

```yaml
switch:
  - platform: vitohome
    name: "Neptun ZP"
    address: 0x7953         # block base
    length: 2               # whole block
    byte_offset: 0          # selects block mode
    bit_mask: 0x01          # Vitosoft BitPosition 7, BitLength 1 (MSB-first)
```
 Nothing
else is sent between the read and the write, so two quick changes to fields of
the same byte cannot undo each other. If the read fails, **nothing is written**.
After the write, the usual read-back also checks that the bits outside the mask
did not change, and logs a warning if they did (a device-side change inside the
window, or a write the device did not take as sent). Under GWG, an EEPROM
address that answers `FF FF` ("not in use") is refused rather than merged into.
Details: `docs/design_notes.md` §5b.

**Why a mask.** A mask has no bit-numbering convention. Vitosoft numbers
`BitPosition` from the most significant bit, vcontrold numbers `<bit>` from the
least significant one (design notes §8e); a value copied from the wrong source
would silently select the neighbouring field. `0xF0` means the high nibble
whoever wrote it. `scripts/gen_catalog.py` converts Vitosoft's numbering once and
prints the original next to the mask:

```yaml
    bit_mask: 0xF0  # Vitosoft BitPosition 0, BitLength 4 (MSB-first)
```

**Rules** (all checked at `esphome config` time):

- the mask is one run of adjacent bits (`0x0C`, `0xF0`; not `0x0A`, not `0`);
- the field is one byte: `length: 1`, or `byte_length: 1` with `byte_offset`;
- every value fits the field (`0..15` for `0xF0`, `0..1` for `0x08`); a `number`'s
  `min_value` / `max_value` are checked after the converter scale;
- `signed` cannot be combined with `bit_mask` (fields are decoded unsigned);
- on `number`, `select` and `switch` the read and the write must be the same
  register: no `state_address` (a field inside a block uses block mode, with
  `address` the block base).

`binary_sensor` keeps its own meaning of `bit_mask`: ON when **any** masked bit is
set. That is well defined for any mask, so it is not restricted to contiguous
ones.

**ASCII** (`type: ascii`): each raw byte is one character. A NUL byte
terminates the string, trailing spaces are trimmed, and any non-printable byte
becomes `?`, so a bad read yields a safe string rather than control characters.
Used for part and serial numbers.

**UTF-16** (`type: utf16`): the payload is a UTF-16LE byte-string decoded to UTF-8 (Vitosoft `HexByte2UTF16Byte`). `length` is the field width in **bytes** and must be even; a `0x0000` or `0xFFFF` unit terminates, and trailing spaces are trimmed. With `byte_offset: 2` the block's first two bytes are `0x00` and the label's length in characters, and that count bounds the decode: the fill after the label is not one pattern (a run of `0xFF`, or `FF 00 00 FF` repeated, which forms no `0xFFFF` unit). Used for the editable heating-circuit labels (`Beschriftung_HK1..3`), emitted read-only (display); round-trip editing would need a `text:` platform with a UTF-16 encode path.

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

This read/write-address split is available on `select`, `switch` and `number`
alike: all three read (poll, read-back, response matching) at `state_address`
when it is given and write to `address`. Combined with `byte_offset` it is the
deprecated interior shape above; a field inside a block uses block mode
instead.

**Schaltzeiten (`text`).** Each entity is one weekday's switching-time program
-- a fixed 8-byte block (four ON/OFF switch-point pairs) at `address`, read and
written at the same register so the hub's read-back re-reads exactly what was
sent. The wire bytes encode/decode to a canonical
`"HH:MM-HH:MM HH:MM-HH:MM ..."` string (up to four pairs, space-separated,
`"--"` or a blank pair leaves that slot disabled); an empty string clears the
whole day (all bytes `0xFF`). `24:00` is accepted as an OFF time only
(`"00:00-24:00"` = on all day, `"06:00-24:00"` = on until midnight; wire byte
`0xC0`), and equal ON/OFF times are kept (`"00:00-00:00"` = off all day). A
malformed input string is rejected in full and logged -- partial writes never
reach the device. Minutes are rounded down to the device's 10-minute grid on
write (`06:07` is stored as `06:00`), with a warning that names the program as
written. A byte read from the device that is no time (minute code 6 or 7, an
OFF time past 24:00) shows as `!XX`, its hex value, with a warning; such a program
cannot be written back as shown, so enter it in full. `read_back` (default `true`, optional
`update_interval`) re-reads immediately after the write ACK, so the Home
Assistant state snaps to the accepted grid value instead of sitting on the raw
input for one poll cycle. There is currently no
`state_address`/`address` split for `text` -- read and write always share the
one configured `address`.

**Validation scope.** The encoder validates what the wire format can
represent -- hour 0..23 (plus exactly `24:00` as an OFF time), minute 0..59, at
most four pairs, per-token grammar -- and one rule *within* a pair: OFF must
not precede ON, compared after rounding down to the 10-minute grid (so
`"06:09-06:01"` is accepted as `06:00-06:00`). A reversed pair such as
`"22:00-06:00"` rejects the whole string rather than being written; the
controller's own UI clears such an interval (observed on a Vitotronic 200
KO1B), so writing it would only produce a different program than the one
typed. Relations *between* pairs -- ascending order and non-overlap -- remain
unchecked, as in the independent reference: vcontrold's `setCycleTime`
(`src/unit.c`) performs no ordering validation at all (it does not even
range-check the hour). Whether the device requires ordered or non-overlapping
pairs is unverified on hardware; that scheduling logic belongs in Home
Assistant or with the user, not in this codec.

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

**Force refresh** (`button`): pressing it (HA UI, or `button.press` from an
automation) calls the hub's `refresh_all()`, which resets every registered
entity's `next_due_ms_` to the boot sentinel so the next scheduler tick
queues them all -- the existing queue discipline (dedup, backpressure,
write preemption, front-loaded read-backs) throttles the burst, exactly like
boot. Reads only; in-flight writes are untouched, the fault-event baseline
is unaffected (it fires on change), and repeats within 5 s are debounced in
the hub with a warning so a misfiring automation loop cannot pin the bus.
ESPHome-side automations can call `id(vito).refresh_all()` directly; the
curated example wires the connectivity sensor's off->on edge to it so
slow-tier entities un-stale themselves the moment the link recovers.

## Validation

Length and range are checked at `esphome config` time, before a wrong value can
reach the device:

- The configured `length` must be one of the converter's valid lengths
  (the table above); a mismatch is rejected during validation.
- With `bit_mask`, the mask must be contiguous, the field one byte, every value
  within the field, and on writable platforms the read and write register the
  same (see *Bit fields* above).
- For a writable `number`, the encodable range derived from the converter and
  `length` is checked against `min_value` / `max_value`, using the same range
  check the C++ encode path applies at runtime — so the config-time check and
  the on-device guard agree.

## Conversions that are not modelled

Some Vitosoft conversions still cannot be represented (for example true
floating-point conversions, `Convert4BytesToFloat`). The catalog generator
emits these as commented hints in the generated package rather than decoding
them incorrectly, so you can decide how to handle them by hand.
