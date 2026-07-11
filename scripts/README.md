# scripts

Tooling for producing and maintaining the datapoint catalog. The two tools here
are independent:

- `gen_catalog.py` reads a Viessmann Vitosoft **XML export** and emits a
  ready-to-include ESPHome package. This is the path you use to configure a
  device.
- `migrate_ecnViessmann.cmd` is a **development tool** that converts the
  Vitosoft SQL Server database into a portable SQLite file for offline
  inspection. It is not part of the ESPHome build or of catalog generation.

## `gen_catalog.py` — generate an ESPHome package

Per-device datapoint definitions (address, length, conversion, access type,
enums, units, borders) are not discoverable over the Optolink bus; they come
from Viessmann's Vitosoft data. `gen_catalog.py` reads the XML export of that
data and emits an ESPHome package with each datapoint on the right platform,
the correct converter, length, bit masks, units and a sensible poll interval.

It is stdlib-only and runs in a bare Python install.

### Input

Point `--data` at the directory holding the Vitosoft XML export. The generator
reads up to four files from it:

| File | Provides |
|---|---|
| `DPDefinitions.xml` | Device types, event links, enum value types, units, borders |
| `ecnEventType.xml` | Per-event Optolink address, lengths, bit position, read/write access, conversion |
| `ecnDataPointType.xml` | Identification ranges (group/ident, hardware/software index) used by `--identify` |
| `Textresource.xml` | UI names in the chosen language (optional; see note) |

`DPDefinitions.xml` does not carry datapoint lengths; those live in
`ecnEventType.xml`, so keep the export files together in one directory.

> **Label source (2026 export).** `Textresource.xml` carries UI strings only and,
> in current exports, **none** of the per-event/value display names — so
> `--culture` mainly affects those (mostly absent) names. Enum **option** labels
> are independent: they come from `ecnEventValueType.Description` in
> `DPDefinitions.xml` (pre-resolved German, ~87% coverage), so an enum/`select`'s
> `options:` are readable regardless of `--culture`. Entity `name:` falls back to
> a snake_case derivation of the technical id where no UI name exists.
>
> One `--culture` side effect: the switch-vs-select decision
> (`_boolean_pair`) recognises German/English on-off label pairs only
> (EIN/AUS, ON/OFF, Ja/Nein, ...). Under other cultures a boolean pair whose
> labels are localised differently is emitted as a two-option `select` instead
> of a `switch` -- functionally equivalent, just not a native HA toggle.

### Options

| Option | Purpose |
|---|---|
| `--data <dir>` | The Vitosoft XML export directory |
| `--list-devices` | List the device tokens (and identification ranges) in the export, then exit |
| `--identify <hex>` | Device identification (`group << 8 \| ident`), e.g. `0x20CB`; auto-selects the revision |
| `--hw <hex>` / `--sw <hex>` / `--f0 <hex>` | Hardware / software index / protocol offset, used with `--identify` |
| `--device <token>` | Generate for a named datapoint-type token, e.g. `VScotHO1_72` |
| `--profile {minimal,standard,full}` | How many datapoints to emit (default: `standard`) |
| `--include <regex>` / `--exclude <regex>` | Keep / drop events whose name matches |
| `--[no-]reachable-only` | Emit only datapoints VitoWiFi can read over Optolink; drop `GFA_READ`/`RPC`/`PROZESS`/`KBUS`/`OT` (default: on). `--no-reachable-only` adds them back — they need custom decode |
| `--order {address,group}` | Entity order: `address` (default) or `group` (by the Vitosoft navigation tree, with a section comment per group) |
| `--culture {de,en,fr,it,ru,nl,pl,da,hu,es,tr,lt,cs}` | Language for names/labels (default: `de`). Currently a near-no-op — see the label-source note above |
| `--[no-]device-id` | Emit a `device_id` diagnostic `text_sensor` and suppress the raw `0xF8`–`0xFB` reads (default: on) |
| `--[no-]error-history` | Emit `error_history` entities for the `FehlerHis*` slots (default: on) |
| `--[no-]error-codes` | Attach a fault-code map to those entities (default: on) |
| `--error-code-set {openv,vd200,vd300,union}` | Which fault-code map to attach (default: `vd300`) |
| `--export-all` | Bulk mode: one catalog per token into `--out` (a directory) plus an `index.csv` manifest — see below |
| `--export-filter <regex>` | With `--export-all`: only export tokens whose ID matches (e.g. `^V`) |
| `--export-suffix <ext>` | With `--export-all`: catalog file extension (default: `.yaml`) |
| `--out <file>` | Output file (default: stdout); with `--export-all`, the output **directory** |

The fault-code maps live in [`fault_codes.py`](fault_codes.py) (one module, the
single source of truth): `vd300` is the Vitodens 300-W (B3HA) set and the default
(VScotHO1_72 / "Projekt Neptun" is the Vitotronic 200 controller in that boiler),
`vd200` is the Vitodens 200 (WB2A) set, `openv` is the generic openv/community
map, and `union` merges all three (most-specific manual wins). **Fault-code
semantics are device-variant-specific** — these are a default to verify on the
unit; openv-vs-VD200 disagreements are in `fault_codes.CONFLICTS`, and the VD300
set carries OCR caveats (a few codes to verify against the PDF) in its header.

### Examples

List what the export contains:

```
python3 scripts/gen_catalog.py --data <export-dir> --list-devices
```

Generate for a device by identification (the values the hub logs at boot, e.g.
`Device: 0x20CB (VScotHO1) HW=0x03 SW=0x51`):

```
python3 scripts/gen_catalog.py --data <export-dir> \
    --identify 0x20CB --hw 0x03 --sw 0x51 \
    --profile standard --out my-heater.vitohome.yaml
```

Or name the datapoint-type token directly:

```
python3 scripts/gen_catalog.py --data <export-dir> --device VScotHO1_72 \
    --profile standard --out my-heater.vitohome.yaml
```

### Bulk export (`--export-all`)

`--export-all` runs the generator for **every** device token with the flags you
pass, writing one catalog per unit into `--out` (a directory) and an `index.csv`
manifest alongside. A unit that produces no Optolink-reachable datapoint is
recorded in the manifest and skipped (no file); one bad unit never aborts the
batch.

```
python3 scripts/gen_catalog.py --data <export-dir> \
    --export-all --no-error-codes --out catalogs/
```

`index.csv` maps every token to its file and identification signature (`ident`,
`hw_index`, software-index and F0 ranges), the linked-event and emitted-entity
counts, and a `status` (`ok` / `skipped: …` / `error: …`). Use it to find the
file for a given unit by its `ident` or `token`.

Because fault-code semantics are device-variant-specific, `--export-all`
attaches the *same* default map (`vd300`) to every unit — pass `--no-error-codes`
for a neutral bulk export, or regenerate an individual unit with the right
`--error-code-set`. The committed result of this command lives in
[`example/catalogs/`](../example/catalogs/).

### Using the output

Include the emitted file from your device YAML and define the `vitohome:` hub
(and `uart:`) yourself:

```yaml
packages:
  heater: !include my-heater.vitohome.yaml
```

Every emitted entity is `disabled_by_default: true`, so you opt in to each one
from Home Assistant. Always run `esphome config`, then `esphome compile` /
`run`, before relying on a value — not every address answers on every firmware.

## `migrate_ecnViessmann.cmd` — Vitosoft database to SQLite (development tool)

A one-time Windows utility that exports the Vitosoft SQL Server database
(`ecnViessmann.mdf`) to a portable SQLite file. If you have the Vitosoft XML you
already installed Vitosoft, which ships a SQL Server instance, so the engine is
already present; this script uses it once to produce a SQLite copy you can query
offline without SQL Server.

This is a maintenance and inspection convenience for working with the catalog
data. It is not consumed by `gen_catalog.py` (which reads the XML export) and is
not part of the ESPHome build.

**Prerequisites:** `sqlcmd`, `sqlite3`, the export tool, PowerShell, and the two
companion scripts in this folder (`verify_counts.ps1`, `verify_encoding.ps1`).

**Configuration:** set the source `.mdf` / `.ldf` paths and the tool paths in
the configuration block at the top of the script before running. The script
operates on **copies** of the database files and detaches (never drops) the
working database, so the source files are never at risk.

**Verification:** after export it checks per-table row counts
(`verify_counts.ps1`) and text encoding (`verify_encoding.ps1`); the encoding
check catches UTF-16 to UTF-8 transcoding errors that a row-count comparison
cannot detect. Both exit non-zero on a discrepancy.
