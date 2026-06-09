# VitoHome — Development Notes

Project-specific notes for the `vitohome` external component: where the
per-device data comes from, the protocol facts the component depends on, and the
decode/validation traps that shaped the current design.

These facts were collected from the
[InsideViessmannVitosoft](https://github.com/SoulSolistice/InsideViessmannVitosoft)
reverse-engineering write-up (`VitosoftCommunication.md`, `VitosoftSoftware.md`,
`VitosoftXML.md`) and **verified against VitoWiFi's actual source at the pinned
commit** (`edc059a7`, `src/Datapoint/Converter.{h,cpp}`). Per §0 of the project
guidelines: the reverse-engineering docs are dated (see §6 below), so anything
version-sensitive here is stated as verified-against-source, not from memory.

---

## 1. Where datapoint definitions come from

A `vitohome` sensor needs four facts per datapoint: `address`, `length`,
`converter`, and (for `binary_sensor`) `byte_offset`/`bit_mask`. These are
per-heater-model and are **not** discoverable over the bus — they come from
Viessmann's own data:

- The **openv wiki** is the practical, community-maintained source (and is what
  `example/hardware-test.yaml` points at for its placeholder addresses).
- The authoritative source is the **Vitosoft XML export**, parsed by the
  InsideViessmannVitosoft scripts. For a given model they emit exactly the inputs
  this component consumes: the event `Address`, `Conversion`, `FCRead`, access
  `Type`, byte/bit positions, value enums, units, and borders.

The 16-bit Optolink address is the hex **after the `~`** in the event address
string: `Outside_Temp~0x0800` → `address: 0x0800`. The token before the `~` is
just the internal name.

---

## 2. Optolink protocol facts (→ `validate_uart_`)

From `VitosoftCommunication.md`, confirmed by how the component behaves:

- **4800 8E2 is mandatory.** The component hard-fails in `validate_uart_()` on
  any of baud/data-bits/stop-bits/parity rather than emitting silent bus errors.
  `example/hardware-test.yaml` sets these explicitly.
- **VS2 (a.k.a. P300) only, for now.** KW (VS1) and GWG are separate protocols
  with different framing and callback shapes; they are Stage-2 work, which is why
  the `protocol:` key is validated (`P300`/`VS2` → VS2) but the VS2 template is
  hardwired in `to_code`.
- **Function codes.** The `FCRead`/`FCWrite` strings in the Viessmann data map to
  numeric Optolink function codes (`Virtual_READ` = 1, `Virtual_WRITE` = 2,
  `Remote_Procedure_Call` = 7, …). Stage 1 is read-only and only uses
  `Virtual_READ`. The event **`Type`** field (1 = read-only, 2 = read/write,
  3 = write-only) is what should gate which datapoints become writable entities
  (`number`/`select`) once the encode path lands in Stage 2.

---

## 3. Converters: VitoWiFi vs. Viessmann — mind the gap

This is the single most important thing to keep in view when adding datapoints.

VitoWiFi at the pinned SHA exposes **exactly four** converters
(`src/Datapoint/Converter.h`):

| VitoWiFi converter | result member | valid length(s) | sign of raw read |
| ------------------ | ------------- | --------------- | ---------------- |
| `noconv`           | unsigned int  | 1, 2, 4         | **unsigned**     |
| `div10`            | `float`       | 1, 2            | signed           |
| `div2`             | `float`       | 1               | signed           |
| `div3600`          | `float`       | 4               | unsigned         |

Viessmann's XML `Conversion` vocabulary is ~50 names (`VitosoftXML.md`). Only a
few have a direct VitoWiFi equivalent:

| Viessmann `Conversion` | VitoWiFi converter | notes |
| ---------------------- | ------------------ | ----- |
| `NoConversion`         | `noconv`           | raw bytes as an **unsigned** integer |
| `Div2`                 | `div2`             | signed, length 1 |
| `Div10`                | `div10`            | signed, length 1 or 2 (negatives OK — temps) |
| `Sec2Hour`             | `div3600`          | value/3600; VitoWiFi doesn't round to 2 dp (use `accuracy_decimals`). **Not exposed in the component yet.** |

**Everything else has no direct converter.** What to do depends on the
conversion's *result type* (the `VitosoftXML.md` conversion table is the
authority for which is which):

- **Double / scaling** (`Div100`, `Div1000`, `Mult2/5/10/100`, `MultOffset`,
  `MultOffsetBCD`): read raw with `noconv` and apply an ESPHome `filters:` step
  (e.g. `Div100` → `- multiply: 0.01`). **Caveat:** `noconv` decodes *unsigned*,
  so this is only correct for non-negative values. A signed raw value needs a
  dedicated converter or explicit sign handling — `noconv` + `multiply` will turn
  a negative reading into a large positive one.
- **String** (`Time53`, `IPAddress`, `FixedStringTerminalZeroes`): cannot be a
  numeric `sensor` at all → needs a `text_sensor` with custom decoding.
- **ByteArray** (`RotateBytes`, `HexByte2*`, `Phone2BCD`): custom decoding.
- **DateTime** (`DateBCD`, `DateTimeBCD`, `DayToDate`): custom decoding.
- **`Convert4BytesToFloat`**: 4 IEEE-754 bytes → `float`. This is **not** the same
  as `div3600` (which reads the 4 bytes as a `uint32`). Reading a true-float
  datapoint via `noconv`-then-cast produces garbage.

---

## 4. `VariantValue` is a tagless union — decode must be converter-aware

`VitoWiFi::VariantValue` (`src/Datapoint/Converter.h`) is a **non-discriminated**
union over `_uint8Val` / `_uint16Val` / `_uint32Val` / `_uint64Val` /
`_floatVal`. Its constructor stores exactly one member and **no tag is kept**, so
reading any *other* member is undefined and returns whatever the bytes happen to
be. Each converter's `decode()` populates a specific member:

- `div10` / `div2` / `div3600` → the `float` member.
- `noconv` → an unsigned-integer member sized by `length`.

So the out-operator you call has to match the converter that produced the value.
`vito_sensor.cpp` does this: `noconv` → `uint8/16/32` selected by `length`,
everything else → `float`. **That is correct today only because every exposed
converter is integer- or float-result.** The result-type column in §3 is the
rule for any future converter — a String/ByteArray/DateTime converter must never
be funneled through `operator float()`.

> Historical note: an earlier revision called `operator float()` on a `noconv`
> value, reading the `float` member over integer bytes and publishing silent
> garbage. That is the canonical instance of this trap, and the reason the decode
> path is converter-aware rather than "just cast to float."

---

## 5. Config-time validation is load-bearing, not belt-and-braces

VitoWiFi guards converter/length combinations with `assert()`, which is **compiled
out under `NDEBUG`** — i.e. in every ESPHome release build. Worse, `noconv`'s
length assert is **commented out in the upstream source**, so it has *no* runtime
guard even in a debug build; an out-of-range length silently decodes as `0`.

That is why `CONVERTER_LENGTHS` in `__init__.py` plus the `sensor.py` cross-check
are the real guard — they turn a silent-wrong-data bug into an `esphome config`
error. Values verified against `Converter.cpp` at the pinned SHA:

```
noconv  -> (1, 2, 4)
div10   -> (1, 2)
div2    -> (1,)
div3600 -> (4,)      # when it is added to CONVERTERS
```

**When bumping the VitoWiFi SHA or adding a converter:** re-read `Converter.cpp`
at the new revision and update `CONVERTER_LENGTHS` to match. The asserts there are
the source of truth; do not infer the constraints. The Python unit tests
(`tests/unit/test_validators.py`) should gain a case for each new converter.

---

## 6. Source-of-truth discipline

- **The library is pinned to an exact commit** (`#edc059a7…`), not a moving
  branch, so an upstream change can't silently alter decode behaviour — or OTA —
  for every device on the next tag. Bump deliberately and re-validate (§5).
- **The reverse-engineering docs are dated.** `VitosoftXML.md` still describes
  per-language `Textresource_de.xml`/`Textresource_en.xml`, but current Vitosoft
  exports ship a single consolidated `Textresource.xml` (languages keyed by
  `CultureId`), and the newer `DPDefinitions.xml` wraps its tables in an
  `ImportExportDataHolder` with an extra `DocumentServerDataSet` the docs don't
  mention. Treat the docs as a map, not the territory: verify datapoint facts
  against a **current** export. (And note the export itself is only correct after
  launching Vitosoft once — the installer ships stale XML that gets regenerated
  from the embedded SQL database on first run.)

---

## 7. The 2026 Vitosoft export, empirically verified

Validated against a full 2026 dataset. Concrete shape, recorded in case it shifts
again:

- **DPDefinitions.xml**: ~203 MB, UTF-8 with BOM. A single `ImportExportDataHolder`
  containing one `ECNDataSet` (~589k rows across 20 tables — `ecnEventType`
  11,582, `ecnEventValueType` 14,613, `ecnEventTypeGroup` 18,018,
  `ecnDataPointTypeEventTypeLink` 104k, `ecnEventTypeEventTypeGroupLink` 148k,
  `ecnDisplayCondition`/`…Group`, `ecnTableExtensionValue` 249k, …) **plus** a
  `DocumentServerDataSet` (mobile-client / error-code extensions:
  `vsmEventTypeExtension`, `vsmErrorCodeMapping`, …). Parses in ~27 s at ~1.6 GB
  peak via a DOM; on a low-RAM target prefer a streaming parse.
- **Standalone files retained, old format**: `ecnEventType.xml` (11,582 rows, root
  `<EventTypes>`), `ecnDataPointType.xml` (399 rows), `ecnEventTypeGroup.xml`.
  These are what `PrintEventTypes.py` / `PrintDatapoints.py` consume directly.
- **Textresource.xml**: UTF-16 LE, consolidated multi-culture (15 cultures,
  `de` = CultureId 1) — but **UI strings only** (~356 per culture).
  `Textresource_de.xml` is a byte-identical copy of it, not a German subset.

### Display names are not in the offline export

The operational catch for any datapoint→entity generation. Every event, value,
and group references its display name via an `@@viessmann.*.name.*` label, but the
strings those labels resolve to are **absent from the 2026 export**: Textresource
carries none of them, and there is no text/localization table anywhere in
DPDefinitions (only `ecnCulture`, the language list). The DocumentServerDataSet's
own names are themselves more `@@` labels. The old large per-language Textresource
files held these strings; 2026 moved them out — most likely fetched from
Viessmann's cloud/DocumentServer at runtime. Only ~125 system/RPC events
(`rpc.xml`, parts of `sysEventType.xml`) carry real localized names.

What every datapoint *does* still carry: a stable **technical identifier**
(`Outside_Temp`, `K00_KonfiAnlagenschemaV300_V333`, …; 10,781 of 11,582 are clean
ASCII tokens) plus address, length, conversion, FCRead, access mode, the
value/enum structure, units, and borders — the full technical spec the component
needs. The display condition (`HIDDEN:(…)`) is a Vitosoft-UI relevance hint, not
a protocol concern, and does not feed the component.

### Naming strategy: entity `id` vs friendly `name`

Drive the two ESPHome fields independently:

- **`id:` ← technical identifier.** Deterministic, ASCII, stable across firmware —
  exactly what a C++/entity id wants. It is the join key back to the Viessmann
  data and should always be carried, even when a friendly name exists.
- **`name:` ← friendly name.** Use the translation when one is available;
  otherwise derive it from the technical id by turning `_` into spaces. The
  derivation is deliberately light: it cleans snake_case (`Outside_Temp` →
  "Outside Temp") but leaves camelCase / coding-prefix compounds (`…A1M1`,
  `K00_…`, `WW`, `RT`) largely intact, so a real translation source (the openv
  wiki, or a recovered/cloud localization) is still preferable where clean German
  labels matter. `PrintEventsForDatapoint.py` implements this fallback
  (`_friendly()`) and always prints the technical id in brackets next to the
  friendly name, so the id is never lost.

---

## 8. Open items / Stage-2 touchpoints

- `CODEOWNERS` still holds the `@yourhandle` placeholder.
- `README.md` is a stub.
- Encode/write path: `Virtual_WRITE`, `number`/`select` platforms, with
  writability gated on the event `Type` field (§2).
- Expose `div3600` (length 4) once a `Sec2Hour` datapoint is wired and tested.
- KW (VS1) / GWG protocol support (separate templates, §2).
- Optional: auto-identify the connected unit by reading group/controller
  identification at `0xF8`/`0xF9` and matching against the `Identification` /
  `IdentificationExtension` fields in `ecnDataPointType` (see `VitosoftXML.md`).
