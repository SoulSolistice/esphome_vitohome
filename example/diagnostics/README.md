# Field programs: probe and audit

One fixed program per protocol that you flash, run and send back, so every
report from every unit reads the same way. Each program is an ESPHome
package: your file keeps the board, the UART, the network and the secrets,
and includes the program.

| Program | Runs | What it does |
|---|---|---|
| `probe-kw.yaml`, `probe-p300.yaml`, `probe-gwg.yaml` | 15 to 20 min, on "Probe: run" | Sweeps one setting at a time: the link's timing points and windows, read lengths, the access modes, the baud rate (GWG), and timed frames for the controller's own limits. One report line per step. |
| `audit-kw.yaml`, `audit-p300.yaml`, `audit-gwg.yaml` | hours, overnight | The link at its defaults under a steady poll load. The counts since boot every 10 min; the recorder dumps every retry, timeout and error. |

Every program is read-only. The probes send their reads as timed frames
too, which needs `raw_access: unrestricted` and `unsafe_raw_writes: true`;
the program sends only the frames listed in its header. A probe step parks
the link, so Home Assistant's values pause while it runs. Flash a program
instead of your usual config.

## Your file

```yaml
esphome:
  name: viessmann-probe

esp32:                     # or esp8266:, as for your usual config
  variant: esp32c3
  board: esp32-c3-devkitm-1
  framework:
    type: esp-idf

wifi:                      # or ethernet:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
ota:
  - platform: esphome

logger:
  baud_rate: 0             # the UART belongs to the Optolink

uart:
  id: optolink_uart        # the programs look for this id (var uart_id)
  rx_pin: GPIO5
  tx_pin: GPIO2
  baud_rate: 4800
  data_bits: 8
  parity: EVEN
  stop_bits: 2

packages:
  program:
    url: https://github.com/SoulSolistice/esphome_vitohome
    ref: main
    files:
      - path: example/diagnostics/probe-kw.yaml
        vars:
          vitohome_source: github://SoulSolistice/esphome_vitohome@main
          # only what differs from the defaults, for example:
          # block_address: "5525"
    refresh: 0s
```

Use the same ref for `ref:` and in `vitohome_source`. The report header
names the source and the commit the firmware was built from, and two reports
compare only at the same commit. A local
checkout works too: `!include {file: ../path/example/diagnostics/probe-kw.yaml,
vars: {vitohome_source: ../path/components}}`.

## The datapoints

Each program reads only what every unit of its protocol serves, by default
the identification the hub reads at boot. Every address is a var; give your
own in `vars:` when your unit serves other ones.

| Var | Programs | Default | Read |
|---|---|---|---|
| `ident_address`, `ident_length` | all | `00F8`, 4 (GWG: `F8`) | the identification (GWG: virtual) |
| `block_address` | KW, P300 | `00F8` | the reads of 1 to 16 bytes (probe), 8 bytes (audit, `block_length`) |
| `gfa_address`, `gfa_length` | KW, P300 | `7650`, 1 | the burner control (GFA, `6B`) |
| `missing_address` | KW, P300 probes | `FFFF` | an address the unit does not serve |
| `physical_address`, `eeprom_address`, `be_address`, `port_address` | GWG | `05`, `17`, `00`, `00` | one address in each access mode |
| `uart_id` | all | `optolink_uart` | |
| `poll_interval`, `stats_interval` | audits | 5 s, 10 min | |
| `recorder_size`, `recorder_persist` | audits | 256, false | ESP32 (not C2/C61): up to 512 entries kept across a restart |
| `log_frames` | audits | false | every frame in the log; a larger log |

The probes take raw-spec hex without `0x` (`raw_op.h`); the audits take
`0x` numbers. Sensors in your own file are polled along with an audit's.

## Running and sending

Capture the log from the start, for example `esphome logs my-unit.yaml >
run.log`, or download it from the dashboard. For a probe, press "Probe: run"
once the log shows the link established, and stop after `probe v1 done`. For
an audit, leave it running and send the log the next morning. "Report now"
logs the header and the counts at any time.

## Report lines

```
report v1 program=probe-kw/1 protocol=KW esphome=2026.11.0 source=github://... sha=0123456789ab device="..."
stats v1 KW enq_to_send:40 s=21 n=20 ok=20 timeout=0 length=0 nack=0 crc=0 error=0 device_error=0 ms=31/38/52 script_ok=0 script_failed=0 link.drained=0 link.retry=0 ...
probe v1 done steps=28
```

- `report`: the program and its version, the protocol, ESPHome, the
  vitohome source and the commit it was built from (`-dirty` for changes not
  committed, `unknown` without git), and the identification.
- `stats` (`VitoHomeComponent::log_stats()`): the counts since the step began
  (probe) or since boot (audit). `n` requests, how each ended, `ms` the time
  from dispatch to an answer (min/avg/max), `script_*` the timed frames, and
  `link.*` the facts the link reports (a retry, the device's NACK, bytes
  drained, ...). Every key on every line.
- `rec dump:` lines (vitohome.recorder): the recorder's entries around a
  failure, as they are.

`python scripts/vitohome_report.py run.log` turns a log into a table, one row
per step; with a second log it compares the two step by step.
