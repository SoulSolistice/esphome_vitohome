# Examples

Configurations for the `vitohome` component. Each file carries a header comment
explaining its purpose in more detail.

Two kinds of file live here. **Standalone configs** — `vscotho1_72.vitohome.yaml`
and the four `vitohome-*.yaml` feature examples — you point `esphome config` at
directly; they read `secrets.yaml` (see `secrets.example.yaml`) and pull their
datapoints from a package. **Packages** — `vscotho1_72.dp.*.yaml` and everything
under `catalogs/` — are include-only fragments meant to be `!include`d into a
config, not validated on their own.

The reference unit throughout is the maintainer's **Vitodens 300-W (B3HA)** with
a **Vitotronic 200** controller — wire ident **`0x20CB`**, Vitosoft token
**`VScotHO1_72`**. That unit is **heating-only** (no DHW cylinder), so DHW
entities in these examples are present as platform coverage and as a template,
and read inert on the reference hardware.

## Reference-unit device configs

| File | What it is |
|---|---|
| `vscotho1_72.vitohome.yaml` | The **flash-and-go** config. Board (ESP32-C3 + DM9051 Ethernet), Optolink `uart:` at 4800 8E2, API/OTA, and `time_sync` (device clock follows Home Assistant). Includes `vscotho1_72.dp.curated.yaml` as its package. Start here, then edit the board/network/secrets for your hardware. |
| `vscotho1_72.dp.curated.yaml` | The **curated** datapoint package it pulls in: a hand-picked monitoring + control set that exercises every platform (`sensor`, `binary_sensor`, `text_sensor`, `number`, `select`, `switch`, `climate`, `text`, `event`, template `water_heater`). Doubles as the starting template you trim to your own unit. |
| `vscotho1_72.dp.complete.yaml` | The **full generated catalog** for the reference unit — `standard` profile, 710 entities, address-ordered, every Optolink-reachable datapoint, all `disabled_by_default`. Same content as `catalogs/vscotho1_72.yaml`, just ordered by address instead of by navigation group. |

## Feature examples

Each is self-contained (validates on its own). To adopt a feature, lift the
relevant block into your device config and keep your own network, `uart:`, and
secrets.

| File | Demonstrates |
|---|---|
| `vitohome-climate.yaml` | Heating circuit 1 as a Home Assistant climate/thermostat card. Betriebsart is exposed as custom presets that bind the split read/write value spaces (write `0x2323`, read `0x2301`) in one place. |
| `vitohome-dhw.yaml` | Domestic hot water as a native HA `water_heater`, built from existing datapoints via ESPHome's template platform — temperature-only, because DHW on/off is folded into the shared Betriebsart register on this controller. |
| `vitohome-timer-clock.yaml` | `time_sync` (hub reads the device clock at `0x088E` and rewrites it on drift) plus per-day Schaltzeiten editing through `text:` entities — one entity per weekday holding that day's switching program. |
| `vitohome-scanner-raw.yaml` | The raw-lane console: one-off `queue_raw_read`/`queue_raw_write`, raw hex with exact 64-bit integer views (no float32 narrowing), and a range sweep from an HA action. Use it to probe addresses. |

## Supporting file

| File | Purpose |
|---|---|
| `secrets.example.yaml` | Copy to `secrets.yaml` alongside your config and fill in the API Noise key and OTA password. These configs use Ethernet (DM9051), so no Wi-Fi secrets are needed. |

## The full catalog set — `catalogs/`

`catalogs/` is the **entire generated catalog**: one ready-to-include package
per Viessmann unit, produced from the Vitosoft **2026** XML export by
`scripts/gen_catalog.py --export-all`. It holds **180 device catalogs**
(group-ordered) plus a manifest.

- **`catalogs/index.csv`** — the manifest, and the place to **find your unit**.
  It has one row per device token in the export (**399** rows): **180** are
  `status: ok` (a catalog file was written), and **219** are
  `status: skipped: no Optolink-reachable datapoints` (no file — every datapoint
  on that unit needs a non-Optolink access method). Columns:

  | Column | Meaning |
  |---|---|
  | `file` | Catalog filename to include (blank when skipped) |
  | `token` | Vitosoft device token, e.g. `VScotHO1_72` |
  | `ident` | Identification hex, `group << 8 \| ident`, e.g. `0x20CB` |
  | `hw_index` | Hardware index |
  | `sw_lo` / `sw_hi` | Software-index range this catalog covers |
  | `f0_lo` / `f0_hi` | Protocol-offset (`0xF0`) range |
  | `events` | Datapoints linked to the unit in the export |
  | `entities` | Entities actually emitted (after profile/filters) |
  | `bytes` | File size |
  | `status` | `ok`, `skipped: …`, or `error: …` |
  | `ext_raw` | Raw identification-extension field |

  Look up your unit by `ident` (the value the hub logs at boot) or `token`, then
  include the `file` named in that row.

- **`catalogs/README.md`** — a short pointer stub for the directory.

> **Fault codes in the bulk set.** `--export-all` attaches the default `vd300`
> (Vitodens 300-W B3HA) fault-code map to *every* unit. Fault-code semantics are
> device-variant-specific, so for any unit other than the reference one,
> regenerate that catalog individually with the right `--error-code-set` — see
> [`scripts/README.md`](../scripts/README.md).

## Using a catalog

Include the file you want as a package and define the hub yourself (or start
from `vscotho1_72.vitohome.yaml`, which already does):

```yaml
packages:
  heater: !include vscotho1_72.dp.curated.yaml   # or catalogs/<your-unit>.yaml
```

Every generated entity is `disabled_by_default: true`, so you opt in to each one
from Home Assistant. Always run `esphome config`, then `esphome compile` / `run`,
before relying on a value — not every address answers on every firmware.
