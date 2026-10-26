# Examples

Configurations for the `vitohome` component. Each file carries a header comment
explaining its purpose in more detail.

## Reference-unit device configs / user curated

The official Viessmann XML catalogs are not complete and datapoints that are
supported by the unit may not be included in the automatically generated
configuration files.

[`example/user_curated`](example/user_curated/) contains contributed device
configurations that can serve as a starting point.

Contributions there go in `example/user_curated/<device>/` as
`<device>.vitohome.yaml` (the full device config) plus `*.dp.*.yaml` datapoint
packages, and CI (`scripts/validate_user_curated.sh`) checks that:

- the device file takes everything from this repo through the
  `vitohome_repo` / `vitohome_ref` substitutions (`url: ${vitohome_repo}`), so
  CI can validate the files in the pull request rather than what is on GitHub;
  no other sources;
- credentials use `!secret` with the names in
  [`secrets.example.yaml`](secrets.example.yaml);
- `esphome config` passes on the ESPHome version pinned in
  `requirements.txt`, so keep `min_version` at or below it.

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
`scripts/export_catalogs.sh`. It holds **201 device catalogs** (group-ordered,
GWG units included) **in two languages**, with the same file names in both:

- **`catalogs/de/`** — German entity names, option labels and fault texts;
- **`catalogs/en/`** — English, German where Vitosoft has no translation.

Units and entity types are identical in both. Home Assistant derives entity
IDs from the names, so pick a language once: switching an installed catalog to
the other language creates new entities. Other languages are one
`gen_catalog.py --culture` away (see [`scripts/README.md`](../scripts/README.md)).

> **Upgrading from the single-language set.** The previous `catalogs/*.yaml`
> files had technical names ("TiefpassTemperaturwert KTS"); the new ones carry
> Vitosoft's display names ("Kesseltemperatur"). The new path
> (`catalogs/de/…`) makes this a deliberate step: Home Assistant will see new
> entity IDs for every renamed entity.

> :warning: **Warning**
> Do not blindly add full catalogs since ESPHome registers all data points
> regardless if they are set to `disabled_by_default: true` or not.
> Especially on resource-constraint devices this may lead to unwanted
> behavior.

- **Find your unit with the [catalog finder](https://soulsolistice.github.io/esphome_vitohome/)**: paste the
  `Device` line the hub logs at boot, or follow the link the hub logs right
  after it. The finder applies the same matching rules as
  `gen_catalog.py --identify` and tells you when a match is only approximate.

- **`catalogs/<lang>/index.csv`** — the manifest behind the finder, one per
  language (they differ only in the `bytes` column).
  It has one row per device token in the export (**399** rows): **201** are
  `status: ok` (a catalog file was written) and **198** are
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
  | `gfa_slots` | Burner-control (GFA) history slots in the catalog; such a catalog takes a `gfa/` companion |

  Reading it by hand takes three steps, and the device id alone is not enough
  (most ids have several rows):

  1. Filter by `ident`, the 4-digit value the hub logs (e.g. `0x20CB`).
  2. Pick the row whose `sw_lo`..`sw_hi` range contains your software index.
     The hub logs it in **hex** (`SW=0x51`); the columns are **decimal** (81).
     If no range contains it, take the row with the highest `sw_lo` below it.
     Use `hw_index` only when the id lists more than one value; a logged `HW`
     that does not occur for the id is ignored.
  3. If several rows remain, they differ by `f0_lo`..`f0_hi`, the value at
     address `0x00F0`, which the hub does not read.

- **`catalogs/resolve.json`** — the same mapping, precomputed for every device
  id, hardware index and software index, for both languages. The finder reads
  this file; `scripts/export_catalogs.sh` writes it together with the two
  `index.csv` files.

- **`catalogs/README.md`** — a short pointer stub for the directory.

- **`catalogs/<lang>/gfa/chip_<XX>.yaml`** — burner-control (GFA) fault texts,
  one small companion per chip. Its texts depend on the chip the unit reports at
  `0x7650`, not on the device type, so they cannot be part of a catalog.

> **Fault texts.** Every catalog carries Vitosoft's own fault texts for its
> device (or the generic Vitotronic set where the device has none and is a
> Vitotronic); units without either show the raw code. For the burner-control
> (GFA) history, include the companion for the `GFA=0x..` the hub logs at boot
> next to the catalog; the catalog finder link already carries the chip and
> shows the line. See [`scripts/README.md`](../scripts/README.md).

## Using a catalog

Include the file you want as a package and define the hub yourself (or start
from `vscotho1_72.vitohome.yaml`, which already does):

```yaml
packages:
  heater: !include vscotho1_72.dp.curated.yaml   # or catalogs/de/<your-unit>.yaml
  faults: !include vscotho1_72.dp.faults.yaml    # curated set only: fault histories + texts
  # gfa: !include catalogs/de/gfa/chip_24.yaml   # GFA texts for chip 0x24 (needs gfa_fehler_01..20)
```

Every generated entity is `disabled_by_default: true`, so you opt in to each one
from Home Assistant. Always run `esphome config`, then `esphome compile` / `run`,
before relying on a value — not every address answers on every firmware.
