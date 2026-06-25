# esphome_vitohome

An [ESPHome](https://esphome.io) external component for Viessmann heating
controllers over the Optolink (optical) interface. It speaks three Optolink
protocols — P300/VS2, KW/VS1 and GWG — and exposes controller datapoints to Home
Assistant as `sensor`, `binary_sensor`, `text_sensor`, `number` and `select`
entities.

vitohome targets ESP32 boards and works under both the ESP-IDF and Arduino
frameworks. It is developed and tested against a VScotHO1 unit (device
`0x20CB`). It implements the P300 (VS2) protocol (the default) and KW (VS1),
both confirmed on a VScotHO1. GWG is also selectable via the `protocol:` option
but remains **untested**. Selecting a non-default protocol logs a warning at
compile time, and the component fails fast at start-up if the configured
protocol doesn't establish a link. Feedback from anyone running GWG is welcome.

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
`text_sensor`, `number`, `select`) accepts, and how a raw Optolink payload
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
