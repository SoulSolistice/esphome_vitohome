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
| `DPDefinitions.xml` | Device types, event links, enum value types, units, borders; from its table extensions (`ecnTableExtensionValue`), the `FunctionValue` a procedure without `PrefixWrite` is called with |
| `ecnEventType.xml` | Per-event Optolink address, lengths, bit position, read/write access, conversion |
| `ecnDataPointType.xml` | Identification ranges (group/ident, hardware/software index) used by `--identify` |
| `Textresource.xml` | ~350 UI strings per language (optional) |
| `Textresource_<culture>.xml` | Entity names, option labels and fault texts, one language per file (optional, but needed for readable names; see note) |

`DPDefinitions.xml` does not carry datapoint lengths; those live in
`ecnEventType.xml`, so keep the export files together in one directory.

> **Label source.** The slim `Textresource.xml` that comes with the definition
> export carries UI strings only (~350 per language) and **none** of the entity
> names. The full `Textresource_<culture>.xml` files of a Vitosoft installation
> (about 30 MB each, 18 languages, e.g. from the installer's
> `data\OFFLINE\…\` folder) carry all of them: entity names, option labels and
> 35,640 fault texts. Put them next to the other files. They are layered per
> label: the slim file < `Textresource_de.xml` < `Textresource_<culture>.xml`,
> and a label missing or empty in the chosen language falls back to German.
> Without them, entity `name:` falls back to the technical id with `_` turned
> into spaces, and options to `ecnEventValueType.Description` (pre-resolved
> German, ~87% coverage).
>
> German catalogs keep `Description` first for options: it is more specific than
> the shared UI label (per heating circuit) and has no hyphenation artefacts.
> Other languages use their translation, German where it is missing. The
> switch-vs-select decision is always made on the German labels, so a catalog's
> entity types do not depend on its language.
>
> **Units never come from the text files.** A unit label is translated
> (`kWh` is "кВтч" in Russian), sometimes empty, sometimes not a symbol at all
> (`ecnUnit.dBm` is "-dBm" in German). [`units.py`](units.py) maps every
> Vitosoft unit token to its Home Assistant / SI symbol and device class, so all
> languages carry the same units. A sign carried in the unit is applied as a
> filter: the RF level bytes are stored as magnitudes and become `dBm` with
> `multiply: -1` (unverified on hardware).

### Options

| Option | Purpose |
|---|---|
| `--data <dir>` | The Vitosoft XML export directory |
| `--list-devices` | List the device tokens (and identification ranges) in the export, then exit |
| `--identify <hex>` | Device identification (`group << 8 \| ident`), e.g. `0x20CB`; auto-selects the revision |
| `--hw <hex>` / `--sw <hex>` | Hardware / software index, used with `--identify` |
| `--f0 <value>` | F0, Vitosoft's extended identification at `0x00F0`, used with `--identify`: decimal as Vitosoft writes it (`30`), or the four hex digits the hub logs as `F0=0x1E00` (two bytes, little-endian: 30) |
| `--device <token>` | Generate for a named datapoint-type token, e.g. `VScotHO1_72` |
| `--profile {minimal,standard,full}` | How many datapoints to emit (default: `standard`). `full`: everything Vitosoft lists for the device. `standard`: the same without LON network-variable inputs (`nvi*`), which only a LON master fills -- without a LON module they read as `0xFF` "invalid" -- and without the LON-bound remote procedure readings (`access: rpc`: the LON participant list, a Vitocom's LON identity). `minimal`: writable values and scaled measurements only |
| `--include <regex>` / `--exclude <regex>` | Keep / drop events whose name matches |
| `--[no-]reachable-only` | Emit only datapoints VitoWiFi can read over Optolink; drop `GFA_READ`/`RPC`/`PROZESS`/`KBUS`/`OT` (default: on). Under P300 a `Remote_Procedure_Call` that is a reading -- read-only, one record, an answer of known size -- is kept and emitted with `access: rpc` (and `prefix:` where it has a `PrefixRead`); a procedure that takes no value becomes a `button` of `type: action` (called with its `PrefixWrite`, else its `FunctionValue`), disabled by default under a warning, in every profile but `minimal`; and a heat pump's RPC fault buffer is fanned out into one raw slot per record (the record index as `prefix:`). `--no-reachable-only` adds them back — they need custom decode |
| `--order {address,group}` | Entity order: `address` (sorted by Optolink address) or `group` (by the Vitosoft navigation tree, with a section comment per group). Default: `group` with `--export-all`, `address` otherwise |
| `--culture {cs,da,de,en,es,fr,hr,hu,it,lt,nl,no,pl,ro,ru,sk,sv,tr}` | Language for entity names, option labels and fault texts (default: `de`); see the label-source note above. Units do not change. Home Assistant derives entity IDs from names, so changing the language of an installed catalog creates new entities |
| `--[no-]menu-datapoints` | Also take the datapoints a device carries only in its own Vitosoft menu tree, not in its direct datapoint list (default: on); see *Where a device's datapoints come from* below |
| `--[no-]device-id` | Emit a `device_id` diagnostic `text_sensor` and suppress the raw `0xF8`–`0xFB` reads (default: on) |
| `--[no-]partial-writes` | Emit writable fields that are smaller than their datapoint -- bit fields (`bit_mask`) and fields inside a block (`byte_offset`) -- as `number` / `select` / `switch`; the component writes them as a read-modify-write of the whole byte or block (default: on). `--no-partial-writes` keeps them all read-only. `--[no-]bitfield-writes` is the older spelling. See *Bit fields* and *Fields inside a block* below |
| `--[no-]error-history` | Emit `error_history` entities for the `FehlerHis*` slots (default: on) |
| `--[no-]error-codes` | Attach a fault-code map to those entities (default: on) |
| `--error-code-set {auto,vitosoft,openv,vd200,vd300,union}` | Which fault texts to attach to the system fault history (default: `auto`); see *Fault texts* below |
| `--gfa-chip <hex>` | The byte read from `GFA_Kennung` (`0x7650`), e.g. `24`: attach Vitosoft's texts for that burner-control chip to the GFA history slots (single device) |
| `--export-all` | Bulk mode: one catalog per token into `--out` (a directory) plus an `index.csv` manifest and a `resolve.json` lookup table — see below |
| `--export-filter <regex>` | With `--export-all`: only export tokens whose ID matches (e.g. `^V`) |
| `--export-suffix <ext>` | With `--export-all`: catalog file extension (default: `.yaml`) |
| `--out <file>` | Output file (default: stdout); with `--export-all`, the output **directory** |

### Fault texts

Fault-code meanings differ by device: the 2026 texts have 88 distinct sets
across 206 device types. With the full `Textresource_<culture>.xml` in `--data`,
the default `--error-code-set auto` attaches Vitosoft's own texts, in the
catalog language:

1. the device's own set (`viessmann.errorcode.<token>.<code>`; 144 of the 166
   catalogs with a system fault history have one);
2. else the generic Vitotronic set, but only for devices Vitosoft gives
   `ErrorType` 1 (Default): it is the boiler meaning, wrong for a Vitobloc CHP
   unit (`ErrorType` 4);
3. else no map: the code shows as raw hex.

Only an export without fault texts at all (slim `Textresource.xml` alone) falls
back to the hand-curated `vd300` map, as before. `--error-code-set vitosoft` is
`auto` without that fallback.

The burner-control (GFA) history is a different code space, keyed by the GFA
chip, which is a property of the installed unit, not of the device type. The
hub reads it at boot (`0x7650`, `GFA_Kennung`) and logs it with the
identification, `GFA=0x24 (NEC)` on VScotHO1_72. The sets differ by chip, not by device type, and 7
codes mean different things on different chips, so there is no safe default.
Two ways to attach them:

- **Companion package** (bulk export): `--export-all` writes
  `gfa/chip_<XX>.yaml` next to the catalogs, one per chip that has texts. It
  extends the catalog's GFA slots by id (`id: !extend gfa_fehler_01`, the map
  written once as a YAML anchor), so it is included next to the catalog:

  ```yaml
  packages:
    heater: !include catalogs/vscotho1_72.yaml
    gfa: !include catalogs/gfa/chip_24.yaml
  ```

  The catalog finder offers the right one when the chip is entered.
- **`--gfa-chip 24`** (single device): the texts are written into the catalog
  itself.

Without either, the GFA slots show raw hex and say how to get the texts. The texts keep Vitosoft's own decimal prefix ("10: Flammenausfall in der
Sicherheitszeit"), the number the boiler display and the manuals use.

The hand-curated maps stay in [`fault_codes.py`](fault_codes.py) for explicit
use: `vd300` (Vitodens 300-W B3HA), `vd200` (Vitodens 200 WB2A), `openv`
(generic openv/community) and `union` (all three, most-specific wins);
openv-vs-VD200 disagreements are in `fault_codes.CONFLICTS`. On VScotHO1_72,
Vitosoft's own set covers all 94 `vd300` codes plus 16 more, and words 48 of
them differently, several with a different meaning (`0x49`, `0x4D`, `0x93`,
`0x9B`, `0x1D`).

### Entity names

Vitosoft UI names are not unique (one "Pegel" per RF participant, "nicht
belegt" for every unused slot), and ESPHome refuses two entities of one platform
with the same name. Every member of such a group gets its `id:` in parentheses,
`Pegel (RF_TeilnehmerInfo_0_Pegel)`, and the catalog header says how many. A
`/` in a name becomes U+2044 FRACTION SLASH, the look-alike ESPHome substitutes
itself (`/` is reserved as a URL separator: a warning now, an error from ESPHome
2027.7.0). The few names the generator makes up itself (`Letzter Fehler`,
`Geraete-Typ`, the weekday `Schaltzeit` entities) are German in a German
catalog and English in every other.

### Where a device's datapoints come from

Vitosoft links datapoints to a device in two places: the direct
device -> datapoint table (`ecnDataPointTypeEventTypeLink`) and the device's
own menu tree (`ecnEventTypeGroup`), through which Vitosoft shows it. 150
devices carry datapoints only in their menus -- for `V200CO1x` its whole
`(A0)..(B5)` coding block, for `CU401B_S` its compressor hours and annual
performance factors -- so the generator takes both. `--no-menu-datapoints`
limits it to the direct table. Details: design_notes §8j.

### Signedness

`div2` and `div10` decode signed in the component. Where Vitosoft declares the
value unsigned (`Parameter` `Byte`/`Int`/`Int4`) **and** its lower border is 0 or
above, the generator emits `signed: false` with the reason as a comment, so a
1-byte 0..127.5 degC temperature does not turn negative above 63.5 degC. The
declared type alone is not trusted: it contradicts the borders on 27 rows,
including the outdoor sensor at `0x0800` (design_notes §8h).

### Bit fields

A datapoint with `BitLength` 1..8 is a field of one byte that other datapoints
share (GWG EEPROM `0x0005` holds eight coding flags). Every such field except a
read-only single bit is emitted with a native `bit_mask` (converters.md, *Bit
fields*), and the mask line cites the export:

```yaml
    bit_mask: 0xF0  # Vitosoft BitPosition 0, BitLength 4 (MSB-first)
```

A writable bit field in a 1-byte datapoint (every GWG bit field) is written as
a read-modify-write of that byte. One in a wider block (the `Neptun_*` outputs
of VScotHO1_72) is written in block mode, see below. Read-only, with a `NOTE`
saying why:

- an inferred scale (fractional stepping on `NoConversion`, e.g. SPR30) or a
  `MultOffset` with an offset -- there is no encode path for either;
- a block that block mode refuses (below);
- everything, with `--no-partial-writes`.

### Fields inside a block

The device writes its whole datapoint: on VScotHO1_72 a 1-byte write into a
2-byte block stored our telegram's checksum byte in byte 1 (design_notes §5b,
§8g). So a writable field smaller than its block -- at `BytePosition > 0`, or
narrower than the block at its base, bit field or not -- is emitted in **block
mode**: `address` = the block base, `length` = the block, `byte_offset` = the
field (even `0`), plus `byte_length` and `bit_mask` where they apply, and a
`NOTE: writes are a read-modify-write of the whole N-byte block`. The component
reads the block, replaces the field, and writes the whole block back.

```yaml
  - platform: vitohome
    name: "Neptun ZP Drehzahl"
    address: 0x7953
    length: 2
    byte_offset: 1
```

It stays read-only, with the reason in its `NOTE`, when:

- the block also holds a **read-only** datapoint -- live data that a block
  write would restore from a stale read (checked over all of the device's
  datapoints, before `--profile` / `--include` filtering);
- the block is larger than 16 bytes (`MAX_RMW_BLOCK`), or the field wider than
  4 bytes;
- it is a command register with a separate state address
  (`COMMAND_STATE_ADDR`);
- `--no-partial-writes` is given.

The weekday Schaltzeiten, written 8 bytes per day as vcontrold does, are
generated by their own path and are not affected.

The `ConversionFactor` / `ConversionOffset` columns on `NoConversion` bit fields
are stale copies of the last `MultOffset` row and are ignored. An enum whose own
option values do not fit its field is a contradiction in the export and becomes
a comment: `SM1_Sensor_*_Status_GWG` declares 7 bits but lists `128` ("Nicht
verwendet", the sibling `0x80` flag), and decoding it as 7 bits showed
"Unterbrechung" on a unit with no SM1 module installed.

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

If several device types share the id and the export gives them no
hardware/software-index range to tell them apart (Dekamatik cascades, M-Bus
meters, which carry serial-style extensions), `--identify` stops and lists them
instead of picking the first one; name the token with `--device`. Earlier
versions silently took the first row.

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
    --export-all --culture en --out catalogs/
```

`index.csv` maps every token to its file and identification signature (`ident`,
`hw_index`, software-index and F0 ranges), the linked-event and emitted-entity
counts, and a `status` (`ok` / `skipped: …` / `error: …`).

`resolve.json` is written alongside. It holds the answer of the `--identify`
matching rules for every device id, hardware index and software index, plus
each token's file and status. The [catalog finder](https://soulsolistice.github.io/esphome_vitohome/)
(`web/` in this repository, published by `.github/workflows/pages.yml`) reads
it, so the page carries no copy of the rules. `tests/unit/test_resolve_index.py`
runs `web/lookup.js` under Node against `Catalog.resolve()`. It does not depend
on the language. To preview the page locally, copy `example/catalogs/resolve.json`
next to `web/index.html` and serve that folder (`python3 -m http.server`).

Bulk mode orders entities by group (`--order group`) unless you say otherwise,
which is how the catalogs in [`example/catalogs/`](../example/catalogs/) were
produced — so a re-export diffs against them entity by entity instead of
reordering every file. Pass `--order address` for the address-sorted layout.
Each unit gets its own fault texts (see *Fault texts*), so a bulk export needs
no `--error-code-set`.

The committed set, German and English, is regenerated in one go with

```
bash scripts/export_catalogs.sh <export-dir>
```

which writes `example/catalogs/de/` and `example/catalogs/en/` (same files,
each with its `index.csv` and `gfa/` companions) and the shared
`example/catalogs/resolve.json` (which also lists the GFA chips), and
refuses to run without the full `Textresource_de.xml` / `Textresource_en.xml`.

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

To facilitate the interpretation of the datapoints' naming, an overview of the
Viessmann nomenclature has been established in
[`docs/datapoint_acronyms.md`](../docs/datapoint_acronyms.md)

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
