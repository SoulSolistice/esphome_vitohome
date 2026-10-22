# esphome_vitohome

An [ESPHome](https://esphome.io) external component for Viessmann heating
controllers over the Optolink (optical) interface. It speaks three Optolink
protocols — P300/VS2, KW/VS1 and GWG — and exposes controller datapoints to Home
Assistant as `sensor`, `binary_sensor`, `text_sensor`, `number`, `select`,
`switch` and `event` entities, plus a hub-fed Optolink connectivity
diagnostic and a force-refresh `button`.

vitohome targets ESP32 boards and works under both the ESP-IDF and Arduino
frameworks. It is developed and tested against a VScotHO1 unit (device
`0x20CB`). It implements the P300 (VS2) protocol (the default) and KW (VS1),
both confirmed on a VScotHO1. GWG is also selectable via the `protocol:` option
but remains **untested**. Selecting a non-default protocol logs a warning at
compile time, and the component fails fast at start-up if the configured
protocol doesn't establish a link. Feedback from anyone running GWG is welcome.

## ⚠️ Read this before writing to your controller

**This project is not affiliated with Viessmann.** It is independent and
unofficial, built by reverse-engineering an undocumented protocol from
community sources. The authors are not heating engineers.

**Reading is low-risk. Writing is not.** A wrong write can leave you with no
heat — and, in winter, frozen pipes — or scalding hot water, disabled frost
protection, a burner cycling far more than it should, or a controller
configuration you cannot restore without a service visit. Lowering the DHW
setpoint below about 60 °C, or disabling a disinfection programme, carries a
legionella risk. **Write down the original value of anything you change, before
you change it.**

**No claims are made regarding the hardware safety chain.** How rigorously a
hardware unit enforces safety-limits or value-sanity is unknown and should not
be relied upon.

**Datapoint names are not authoritative.** Labels here come from
reverse-engineered tables, not from Viessmann documentation, and a plausible
name is not proof that an address means what it says. This project has hit
exactly that: a GWG datapoint labelled *Brennerlaufzeit* (burner operating
hours) returned a **decreasing** value across consecutive polls — a counter
cannot do that, so either the label, the address or the framing was wrong.
Under GWG the same address is genuinely a different datapoint in different
access modes: address `0x01` is five of them across four modes. Read a
datapoint and watch it behave sensibly before you ever write to it.

**Where a write lands is unknown.** We do not know whether a given controller
holds a written value in RAM, in RAM with a periodic commit to non-volatile
storage, or writes straight to EEPROM. GWG is the only protocol that even hints
at this, because it names the access mode — and there, the writable datapoints
are explicitly the EEPROM ones (`EEPROM_WRITE` and `BE_WRITE`; see
`scripts/gen_catalog.py`). EEPROM cells have a finite write endurance, and we
do not know the figure for these controllers.

**So do not put writes in fast-firing automations.** A write on every state
change, or a periodic write "to keep things in sync", can accumulate hundreds
of thousands of cycles a year. Write on deliberate user action or on a real
change, and rate-limit it. This is the most likely way to do lasting damage
with this component, and it is entirely avoidable.

Use at your own risk. There is no warranty; see [`NOTICE.md`](NOTICE.md) for
licensing and trademarks.

## Features

**Entity platforms.** Every ESPHome entity type that maps to a Viessmann
datapoint is supported, so most of a controller can be surfaced without custom
lambdas:

- `sensor` — numeric datapoints (temperatures, hours counters, volume flow,
  pump power, modulation), with a per-converter scale and optional
  single-byte extraction from a larger block (`byte_offset`).
- `binary_sensor` — bit-masked status flags, plus a hub-fed **connectivity**
  diagnostic (`device_class: connectivity`) that reflects the Optolink link
  itself rather than any single datapoint.
- `text_sensor` — six kinds: `enum` (mapped value → label), `error_history`
  (fault-history slot: code byte + timestamp, decoded against a fault-code
  map), `device_id`, `ascii` and `utf16` string fields, and `raw` hex.
- `number` — writable numeric setpoints (setpoints, heating-curve slope and
  level), with min/max/step and the usual ESPHome `mode:` (`box`, `slider`,
  `auto`). `number`, `select` and `switch` also write fields that share their
  byte or block with other datapoints (`bit_mask`, `byte_offset`) as a
  read-modify-write of the whole byte or block, so the neighbours go back
  exactly as read.
- `select` — writable enumerated settings, including devices whose read and
  write addresses differ (`state_address`).
- `switch` — writable on/off registers, likewise supporting a separate
  read-back address (`state_address`).
- `text` — per-day **switching-time programs** (Schaltzeiten): the four
  ON/OFF switch-points of a day edited as a human-readable string, packed
  back into the device's 8-byte binary format.
- `climate` — see below.
- `event` — fires a Home Assistant logbook **event** when a fault-history
  code changes (new code, `cleared`, or `unknown`), complementing the
  polling `error_history` text sensor.
- `button` — a **force-refresh** button that re-queues every datapoint on
  demand (also callable from automations as `id(hub).refresh_all()`).

**Distinctive functionality.** Beyond plain read/write datapoints, the hub
provides several things you would otherwise have to build by hand:

- **Device-clock sync ("boiler NTP").** With a `time:` source and
  `time_sync:`, the hub keeps the controller's own real-time clock aligned
  with Home Assistant / SNTP time. Nothing is written until the time source
  has actually synced since boot (its `on_time_sync` fired); a time the ESP
  merely carried over a reboot is not trusted. It reads the device clock,
  compares it, and writes the corrected time **only when the drift exceeds a
  configurable threshold** — then reads it back to verify. The Viessmann
  weekday byte is written with the device's own convention. So the heater's
  built-in clock (which drives its Schaltzeiten) stays correct without ever
  touching the front panel.
- **Native `climate` entity.** A heating circuit is exposed as a Home
  Assistant climate card: the target temperature drives the room setpoint,
  and the controller's operating modes (Betriebsart) are exposed as presets.
  It handles the real-device wrinkle that the **command** address written to
  set a mode differs from the **state** address read back, mapping each
  read-back value to the preset that produced it.
  Each preset must declare `mode:` (`heat`, `off` or `auto`) — the coarse HVAC
  mode the card shows for it. Several presets may share one (Normal and
  Reduziert are both `heat`); a bare mode call from the HA thermostat card or a
  voice assistant then writes the **first** preset with that mode, so list order
  decides the default and `esphome config` warns naming the winner. Declaring a
  Standby/Frostschutz preset as `off` rather than leaving it to default is the
  point: it used to default to `heat`, which could turn the heating off in
  response to a request to turn it on.
- **DHW as a `water_heater` card.** Domestic hot water can be presented as a
  native Home Assistant water-heater entity built entirely from existing
  datapoints (tank temperature, writable setpoint, effective read-back) —
  see `example/vitohome-dhw.yaml`; no extra C++.
- **Interactive scan console.** A `scan_result` text sensor plus
  `queue_raw_read` / `queue_raw_write` lets you read or write an arbitrary
  address at runtime (`queue_raw_rpc` makes a P300 remote procedure call) and sweep address ranges — useful for identifying
  datapoints on an unknown unit. These interactive operations preempt normal
  polling so the console feels immediate.
- **Boot-time device identification.** The hub reads the controller
  identification (`0xF8`–`0xFB`) once at start-up and logs the family, HW and
  SW index; the result is pushed to any `device_id` text sensor.

**How it reads and writes, safely.** A single prioritized dispatcher shares
the one optical link across every entity: **identification → interactive scan
console → user writes → background clock-sync → routine reads**. A slider drag
or mode change therefore never waits behind a full poll cycle, while the
non-urgent clock sync yields to everything a person is waiting on. Values are
decoded in double precision and narrowed to `float32` only at publish time, so
large counters don't lose resolution; a datapoint reports unavailable only
after several consecutive read failures, not on a single transient glitch.

**Catalog generator.** The repository ships a generator that turns a Viessmann
Vitosoft export into a ready-to-use YAML datapoint catalog for a specific
controller, complete with units, device classes, the device's own fault texts
and the correct converters, in any of Vitosoft's 18 languages. The committed
catalogs come in German and English (`example/catalogs/de/`, `…/en/`) — see
[`scripts/README.md`](scripts/README.md) and the `example/` catalogs.

**Finding the catalog for your unit.** At boot the hub logs the controller's
identification and a link to the [catalog finder](https://soulsolistice.github.io/esphome_vitohome/), for example:

```
Device identification: 0x20CB (VScotHO1) HW=0x03 SW=0x51 GFA=0x24 (NEC)
Find the matching datapoint catalog: https://soulsolistice.github.io/esphome_vitohome/?id=20CB&hw=03&sw=51&gfa=24
```

`GFA=` is the burner-control chip (read from `0x7650` on P300/KW units that have
one). It selects the burner-control fault texts, which the finder adds as a
small companion file next to the catalog.

`F0=0xXXYY` appears on units that have Vitosoft's extended identification at
`0x00F0` (on P300/KW; the link then carries `&f0=XXYY`). The hub passes the two
bytes on raw; the finder reads them as Vitosoft does, one little-endian number
(`F0=0x1E00` is F0 30, the `_30` of `VScotHO1_200_30`), and uses it where a
software-index range covers several variants (the Vitodens/Vitocrossal
`VScotHO1_200*` and `VScotHC1_200*` families at software index `0xC8` and
above) and ignores it everywhere else.

Open the link, or paste the `Device` line into the finder, to get the catalog
file for that controller. The device id alone is not enough: most ids cover
several catalogs that differ by software index. The finder runs in the browser
from a generated table; the ESP does not need internet access. Opening the link
sends the identification bytes in the URL to GitHub Pages, like any page
request; they are not stored by the finder itself.

## What you need

- An ESP32-class board.
- An Optolink read/write head, placed over the optical interface on the
  Viessmann unit and wired to the board's UART. See
  [JuergenLeber/home-assistant-optolink](https://github.com/JuergenLeber/home-assistant-optolink)
  for a nice example.
- A UART configured for **4800 baud, 8 data bits, even parity, 2 stop bits
  (8E2)**. This is mandatory — the hub refuses to start on any mismatch.

## Quick start

Add the component, configure the UART and the hub, and add a datapoint. Pin
selection is board-specific; configure Wi-Fi/Ethernet, `api` and `ota` as you
would for any ESPHome device.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/SoulSolistice/esphome_vitohome
      ref: main
    components: [vitohome]

# 4800 8E2 is mandatory. Adjust the pins for your board.
uart:
  - id: uart_optolink
    rx_pin: GPIO05
    tx_pin: GPIO02
    baud_rate: 4800
    data_bits: 8
    parity: EVEN
    stop_bits: 2

vitohome:
  id: vito
  uart_id: uart_optolink
  protocol: P300            # also KW (VS1), confirmed; GWG has one hardware capture
  update_interval: 60s      # base poll tick
  # identify_device: false  # default true: log the device id (0xF8..0xFB) and
  #                         # burner-control chip (0x7650) once at boot
  # time_id: my_time       # optional: sync the device clock from a time source
  # time_sync:
  #   clock_address: 0x088E # device clock datapoint. NOT constant across
  #                         # Viessmann devices: 0x088E is NRF/Vitotronic (the
  #                         # default); WPR heat-pump controllers (Vitocal:
  #                         # V200WO1A, VBC700_*, VBC702_*, CU401B_*) use
  #                         # 0x08E0. Wrong address = a blind BCD write to a
  #                         # datapoint that isn't the clock, so check yours.
  #                         # Not supported under protocol: GWG (its clock is
  #                         # three 1-byte registers, a different shape) —
  #                         # rejected at config time.
  # raw_queue_size: 0       # scan-console lane slots (~38 B each, reserved once
  #                         # at boot). Default 0: the scan console is a debug
  #                         # tool, so it is opt-in and costs nothing unless
  #                         # used. Set it only if you call queue_raw_read /
  #                         # queue_raw_write or use a scan_result text_sensor:
  #                         # 1–2 for one-off reads, 256 for RANGE SWEEPS
  #                         # (~9.7 KiB — see example/vitohome-scanner-raw.yaml).
  #                         # Time sync does NOT use this lane.

sensor:
  - platform: vitohome
    name: "Outside temperature"
    address: 0x5525
    length: 2
    converter: div10        # signed, divide by 10
    unit_of_measurement: "°C"
    device_class: temperature
    state_class: measurement
    accuracy_decimals: 1
```

This starting config is deliberately **read-only** — one sensor, no writable
entities. That is the right way to begin: confirm the link comes up and that a
datapoint you can verify against the front panel reads correctly, before adding
anything that writes.

The `converter:` above is one preset among several. For the full set of
converters, the options each platform (`sensor`, `binary_sensor`,
`text_sensor`, `number`, `select`, `switch`) accepts, and how a raw Optolink payload
becomes an entity state, see [`docs/converters.md`](docs/converters.md) — the
reference for authoring a datapoint by hand or reading a generated one.

For a full device, generate a package rather than writing every datapoint by
hand — see [`scripts/README.md`](scripts/README.md). The
[`example/`](example/) directory has complete configurations you can start
from.

## How it works

- The Optolink protocol engine under
  [`components/vitohome/optolink/`](components/vitohome/optolink/) implements the
  framing for the configured protocol (P300/VS2, KW/VS1 or GWG) and serves as
  the wire/transport layer.
- The component decodes and encodes raw payloads itself in `decode.h`,
  reading into a 64-bit integer, scaling in `double`, and narrowing to float
  only at the ESPHome state boundary. See
  [`docs/converters.md`](docs/converters.md).
- Per-device datapoint definitions come from Viessmann's Vitosoft data, turned
  into an ESPHome package by `scripts/gen_catalog.py`.
- The hub reads the device identification at boot and logs the device tuple
  and the catalog-finder link, on every protocol, so you can confirm which unit
  you are talking to (`identify_device: false` turns it off).

## Reading and writing

This component can write to a heating controller. Read the warning at the top
of this README ("Read this before writing to your controller") first — in
particular, that a datapoint's *name* is not evidence of what its address does,
and that repeated writes may be wearing an EEPROM cell.

Beyond that, treat configuration with care:

- Always run `esphome config`, then `esphome compile` / `run`, before relying
  on a value. Schema validation does not run code generation, and code
  generation does not run on the device.
- Not every address answers on every firmware. A datapoint that validates and
  compiles may still not respond on your unit.
- Prove a writable datapoint by reading it first. Poll it, change the setting
  at the front panel, and confirm the value tracks — that is the cheapest
  evidence that the address and its scaling are what you think, and it costs
  nothing but a poll cycle.

### GWG only: `access:`

Under `protocol: GWG`, an entity may set `access:` to pick the telegram TYPE
byte. GWG's address space is per access mode, so this is not optional detail:
in Viessmann's own tables, address `0x01` is five different datapoints across
four modes. One mode drives both the entity's reads and its writes.

```yaml
sensor:
  - platform: vitohome
    name: "GWG ident"
    address: 0xF8
    byte_length: 2
    access: virtual        # default: physical
```

Accepted values: `physical` (default), `virtual`, `eeprom`, `xram`, `port`,
`be`, `kmbus_ram`, `kmbus_eeprom`, on `sensor`, `binary_sensor`, `text_sensor`,
`number`, `select`, `switch`, `text` and `climate`. Setting it under any other
protocol is a config error. The two `kmbus_*` modes are read-only — they have
no write telegram type — and writes in them are refused.

**On a writable entity `access:` is required, not optional.** Across all 22
Vitosoft GWG device tokens the writable datapoints are `EEPROM_WRITE` (1082)
and `BE_WRITE` (280) — and *zero* `PHYSICAL_WRITE`. So an omitted mode would
emit a write telegram (`0xC8`) that matches no documented GWG datapoint, at an
address whose meaning is mode-dependent: a silent write to something other than
what you intended. There is no safe default to fall back on, so `esphome config`
rejects it. A generated catalog always carries the right value, so this only
affects hand-written configs.

On a **read-only** entity an omitted `access:` is a warning rather than an
error: Vitosoft emits rows with a blank `FCRead`, and blank legitimately means
physical, so demanding an explicit `access: physical` on those would be
ceremony. The warning names the entity and the assumption, because if physical
is the wrong mode the entity publishes a plausible-looking value read from the
wrong register.

`climate` is the one platform with two datapoints, and they are separate
registers, so each carries its own mode: the top-level `access:` applies to the
setpoint channel (`target_address`), and `operating_mode.access:` to the
Betriebsart channel. Both channels read *and* write, so a `kmbus_*` mode is
rejected on either.

```yaml
climate:
  - platform: vitohome
    name: "Heizkreis 1"
    target_address: 0x06
    access: eeprom              # setpoint channel
    operating_mode:
      address: 0x23
      access: be                # Betriebsart channel — independent of above
      presets:
        - {name: "Normal",  write: 0x04, read: [0x02], mode: heat}
        - {name: "Standby", write: 0x00, read: [0x05], mode: "off"}
```

Note GWG writes remain **unconfirmed on hardware**, and one detail of the write
frame is still undecided; see
[`docs/design_notes.md`](docs/design_notes.md#writes).

You normally will not write these by hand: `scripts/gen_catalog.py` reads the
access mode out of Vitosoft's own `FCRead` field and emits `access:` on every
entity when generating for a `GWG_*` device token.

Only `physical` has been seen on this project's hardware. The other bytes come
from the openv wiki's `Protokoll-GWG` table, and all but `kmbus_ram` (`0x33`)
are corroborated twice over — by vcontrold's GWG macros and by Vitosoft, which
between them poll `virtual`, `eeprom`, `port`, `xram`, `be` and `kmbus_eeprom`
on real units. Details, the full TYPE-byte table and the per-mode evidence are
in [`docs/design_notes.md`](docs/design_notes.md#gwg-access-modes-access).

### P300 only: `access: rpc`

Under `protocol: P300`, `access: rpc` reads a datapoint as a **remote procedure
call** (function code `0x07`) instead of a plain read. A call may carry a
parameter, `prefix:` -- Vitosoft's `PrefixRead`, typically the index of one entry
in a list the controller keeps behind a single address -- so several entities
can share an address and differ only in their prefix. A call without a
parameter simply leaves `prefix:` out:

```yaml
sensor:
  - platform: vitohome
    name: "LON participant 0"
    address: 0xA010
    access: rpc
    prefix: [0x00]          # 1..4 bytes; a single value is fine too
    length: 6               # the entry the controller answers with
    byte_offset: 3
    byte_length: 2
  - platform: vitohome
    name: "Ist-Kesselfolge"   # a cascade controller; no parameter
    address: 0xA104
    access: rpc
    length: 4
```

Accepted on `sensor`, `binary_sensor` and `text_sensor` only. Vitosoft also
marks *procedures* `Remote_Procedure_Call` -- resetting a circuit's parameters,
clearing the fault history -- and nothing on the wire tells a procedure from a
reading: polling a procedure's address runs it, every cycle. So `access: rpc`
belongs on an address the catalog calls a reading, which is what the generator
emits. `length:` and `byte_offset:` mean what they mean for a read; they are not
sent. KW and GWG have no such telegram, so `esphome config` rejects it there.

A procedure is a **`button` of `type: action`**: each press is one call, with
the procedure's parameter where it takes one. It is never polled and never read
back -- reading a procedure would run it again -- and once the controller
acknowledges, every value is re-read so the effect shows up:

```yaml
button:
  - platform: vitohome
    type: action
    name: "Fehlerhistorie Löschen"
    address: 0xA029
    access: rpc
    disabled_by_default: true
```

`scripts/gen_catalog.py` emits `access: rpc` (and `prefix:` where there is one)
for the remote procedure readings in Vitosoft's data. The unit's own readings --
a cascade controller's boiler sequence, an M-Bus module's state -- are in the
default profile. The LON-bound ones (the LON participant list, a Vitocom's LON
identity, LON inputs read by procedure) are answered without a LON module too,
but then hold only the unit's own defaults, so they are in `--profile full` only.
Confirmed on a VScotHO1_72: the unit answers remote procedure calls, and its
participant list reads its own default number and empty slots.

It also emits every **procedure** as an action button, in the default profile
too: clear the fault history, reset a circuit's operating parameters,
**reset all codings to the delivery state**, step a boiler sequence. Every one
is `disabled_by_default` under a warning comment, because a press **runs** the
procedure on the controller, at once and without asking, and nothing here undoes
it. Read the name before you enable one. A procedure that needs a value (a
setter) is not a button and is not emitted. From a lambda, `queue_raw_rpc(address,
parameter)` calls any procedure directly.

A heat pump's **fault history** is a remote procedure array: thirty records
behind one procedure at `0xA801`, each one call with the record index as its
parameter. The generator fans it out like the Vitotronic archive -- "Letzter
Fehler (WPR)" enabled, the other 29 records disabled -- as raw hex until the
record layout is decoded. Details and the one open wire question are in
[`docs/design_notes.md`](docs/design_notes.md#p300-remote-procedure-calls-access-rpc).

## Development

- Python unit tests live in [`tests/unit/`](tests/unit/) (run with `pytest`).
- A host-side C++ harness covers the decode logic and VS2 transactions without
  hardware; see [`tests/native/README.md`](tests/native/README.md).
- A live `esphome compile` / `run` on the target board is the final check for
  any change.

## License and credits

vitohome is licensed under the GNU General Public License v3.0 (see
[`LICENSE`](LICENSE)). It builds on VitoWiFi, InsideViessmannVitosoft and
openv/vcontrold — see [`NOTICE.md`](NOTICE.md) and
[`THIRD_PARTY.md`](components/vitohome/optolink/THIRD_PARTY.md).

This work would not have been possible without the "prior art" that was
and is truly foundational. Many thanks to all of it.
