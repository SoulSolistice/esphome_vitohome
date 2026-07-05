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
  `auto`).
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
  with Home Assistant / SNTP time. It reads the device clock, compares it,
  and writes the corrected time **only when the drift exceeds a configurable
  threshold** — then reads it back to verify. The Viessmann weekday byte is
  written with the device's own convention. So the heater's built-in clock
  (which drives its Schaltzeiten) stays correct without ever touching the
  front panel.
- **Native `climate` entity.** A heating circuit is exposed as a Home
  Assistant climate card: the target temperature drives the room setpoint,
  and the controller's operating modes (Betriebsart) are exposed as presets.
  It handles the real-device wrinkle that the **command** address written to
  set a mode differs from the **state** address read back, mapping each
  read-back value to the preset that produced it.
- **DHW as a `water_heater` card.** Domestic hot water can be presented as a
  native Home Assistant water-heater entity built entirely from existing
  datapoints (tank temperature, writable setpoint, effective read-back) —
  see `example/vitohome-dhw.yaml`; no extra C++.
- **Interactive scan console.** A `scan_result` text sensor plus
  `queue_raw_read` / `queue_raw_write` lets you read or write an arbitrary
  address at runtime and sweep address ranges — useful for identifying
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
controller, complete with units, device classes, fault-code maps and the
correct converters — see [`docs`](docs) and the `example/` catalogs.

## What you need

- An ESP32-class board.
- An Optolink read/write head, placed over the optical interface on the
  Viessmann unit and wired to the board's UART. See https://github.com/JuergenLeber/home-assistant-optolink
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
  protocol: P300            # also KW (VS1), confirmed; GWG selectable but untested
  update_interval: 60s      # base poll tick
  identify_device: true     # read 0xF8..0xFB once at boot and log the device

sensor:
  - platform: vitohome
    name: "Outside temperature"
    address: 0x0800
    length: 2
    converter: div10        # signed, divide by 10
    unit_of_measurement: "°C"
    device_class: temperature
    state_class: measurement
    accuracy_decimals: 1
```

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
- With `identify_device` on, the hub reads the device identification at boot and
  logs the device tuple, so you can confirm which unit you are talking to.

## Reading and writing

This component can write to a heating controller. Treat configuration with
care:

- Always run `esphome config`, then `esphome compile` / `run`, before relying
  on a value. Schema validation does not run code generation, and code
  generation does not run on the device.
- Not every address answers on every firmware. A datapoint that validates and
  compiles may still not respond on your unit.

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
