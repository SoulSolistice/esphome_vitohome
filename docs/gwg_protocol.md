# GWG protocol: field reference

What this project knows about GWG units (Vitodens 200 WB2 class: VR20
controller, LGM29 burner control, `0x2053`/`0x2054`), and where each fact comes
from. It summarises; the reasoning and the code paths are in
[design_notes.md](design_notes.md) §2 (GWG access modes, Bursting, GWG EEPROM,
Identification under GWG, Writes) and §8d/§8e.

Evidence labels used below:

- **HW**: seen on a unit, with the issue that reported it.
- **Vitosoft**: Viessmann's own export (`ecnEventType.xml`,
  `ecnDataPointType.xml`, `sysDeviceIdent.xml`, `Textresource_de.xml`, the SQL
  link tables).
- **Spec**: openv wiki, vcontrold, vogod; not confirmed here.

## Units reported so far

| Issue | Ident | HW | SW | Panel (owner) | Notes |
|---|---|---|---|---|---|
| #13 | `0x2053` | `0x01` | `0x2B` | BES (photo) | first GWG capture, 2026-08-23 |
| #21 | `0x2053` | `0x01` | `0x7A` | not stated | bursting settled; 13 `be` addresses all `0xFF` |
| #35 | `0x2053` | `0x01` | `0x1E` | BES | Vitodens 200 WB2; VR20 and LGM29 replaced years ago; 15 `be` addresses all `0xFF`; no `kmbus_eeprom` answer |

Every unit so far reports HW `0x01`, which Vitosoft maps to BEM. At least two of
them have a BES. See [Identification](#identification).

## Wire format

| Item | Value | Evidence |
|---|---|---|
| Line | 4800 baud, 8E2 | Spec, HW |
| Idle ENQ | `0x05`, every 749–2070 ms (median 1229) | HW #13 |
| Read request | `01 TYPE ADDR LEN 04` after an ENQ; `TYPE ADDR LEN 04` inside a burst | HW #21 |
| Response | LEN raw bytes; no framing, checksum, address echo or error telegram | HW |
| Answer time | 23–53 ms after the request | HW #21, #35 |
| Answer deadline | 250 ms default (`gwgResponseTimeoutMs`, 50–2000 ms from a lambda) | design choice |
| Address | one byte; anything above `0xFF` is rejected at config time | source, §8d |

- **No error answer.** An address a mode does not serve answers `0xFF` (all
  bytes), not a refusal. A silent datapoint does not answer at all.
- **A raised deadline must stay under the idle-ENQ gap** (about 700 ms) for
  one-byte reads. Above it, the next idle ENQ completes a silent read as the
  value 5.
- **Bursting works without the leading `0x01`** (HW #21: 25 of 25 answered
  without it, 0 of 4 with it). A burst-sent read that times out is retried once
  on a fresh ENQ; only an answered retry counts against bursting (HW #35:
  burst stays on with 54 silent datapoints).

## Access modes

Each mode is its own address space; the same address in two modes is two
unrelated registers. The catalog's `access:` comes from Vitosoft's `FCRead`.

| `access:` | Read | Write | Status on hardware |
|---|---|---|---|
| `physical` | `0xCB` | `0xC8` | answers, plausible values (HW #21, #35) |
| `virtual` | `0xC7` | `0xC4` | answers; identification lives here (HW #13, #21, #35) |
| `eeprom` | `0xAE` | `0xAD` | answers in complement pairs (HW #21, #35) |
| `xram` | `0xC5` | `0xC3` | answers (HW #35) |
| `port` | `0x6E` | `0x6D` | answers (HW #35) |
| `be` | `0x9E` | `0x9D` | answers fast, but `0xFF` at every address tried (HW #21, #35) |
| `kmbus_eeprom` | `0x43` | none | never answers, not even late (HW #35) |
| `kmbus_ram` | `0x33` | none | no Vitosoft datapoint uses it; never sent |

### `eeprom`

- One telegram per address. The unit answers a pair: byte 0 is the one's
  complement of byte 1, byte 1 is the value (`b0 + b1 == 0xFF`).
- `FF FF` means "address not in use". A pair that does not add up to `0xFF`
  fails the read.
- The owner of #35 reports that half of `0x00..0xFF` answers `FF FF` and that
  the used part repeats from `0x80` (a 128-byte part). Vitosoft's GWG
  datapoints use `0x01..0x24` only.

### `be`

- Vitosoft describes this space as the panel's: `GWG_BetriebsprogrammBEM`
  (`be` `0x57`) is the "Betriebsprogramm der menuegefuehrten Bedieneinheit",
  i.e. the menu-driven panel (BEM).
- Both units tested answered `0xFF` for every `be` address, including both
  clock locations (BEM `0xF4..0xF6`, BES `0x74..0x76`). On #35 that fits the
  BES, which has knobs and no menu.
- Many `be` datapoints have a same-named `physical` copy (for example
  "Neigung der Heizkennlinie HK A": `physical` `0x65` and `be` `0x20`).
  Plausible values in a log usually come from the `physical` copy. Check the
  frames before calling a `be` read confirmed.
- So far no `be` read has returned data. A `be` write would go to a space
  nothing answers from.

### `kmbus_eeprom`

- These reach the LGM29 burner control over the VR20's KM-Bus. On #35 all 54
  went unanswered, with no late bytes either (late-answer report on, frames
  logged). The owner says the KM-Bus itself works (the pump answers there) and
  suspects the LGM29 needs enabling.
- Cost while silent: about one ENQ gap plus a 250 ms retry per read, so 54 of
  them overran a 60 s poll cycle until poll backoff set in (after three
  cycles).

### Writes

No GWG write has been confirmed anywhere. Vitosoft pairs every writable
datapoint within one mode (`EEPROM_WRITE` 1082, `BE_WRITE` 280,
`PHYSICAL_WRITE` 0). Where the `0x04` goes in a write frame is unknown; both
layouts are switchable at run time (`set_gwg_write_eot_before_payload()`).
Given the `be` results above, an `eeprom` datapoint is the only candidate left
for a first write test, and those are codings.

## Identification

- Four bytes at `virtual` `0xF8`: group, ident, hardware index (HX, `0xFA`),
  software index (`0xFB`). Vitosoft: `sysDeviceIdent.xml`,
  `GWG_SystemIdent_GG/GK/HX/SW`.
- Vitosoft matches `0x2053` plus IdentificationExtension, whose high byte is
  the hardware index:

  | HW | Vitosoft family | Description |
  |---|---|---|
  | `0x01` | `GWG_VBEM_*` | Bedienteil BEM |
  | `0x02` | `GWG_VBES_*` | Bedienteil BES |
  | `0x08` | `GWG_VWMS_*` | BES in the wall-mounting base WMS |
  | `0x10` | `GWG_VBT2_*` | Bedienteil BT2, constant-temperature control |

  `0x2054` is `GWG_BT2` (all software indices).
- **In practice HW does not name the panel.** Units #13 and #35 have a BES and
  report `0x01`. The likely cause is that the value belongs to the VR20, not to
  the panel; #35's VR20 is a replacement part.
- Software index: Vitosoft's "ab Softwareindex 21" is `0x21`. The catalog
  revisions are `_00`, `_03`, `_21`, `_35`, `_36`; an index between two
  revisions uses the lower one (SW `0x1E` → `_03`).
- Vitosoft has no other datapoint that identifies the panel: no product or
  short name, no controller-type field, no "panel present" flag. The
  `Neptun_GWGF8..FA` entries belong to a newer controller reading a GWG's
  codings, not to the VR20.

### BEM and BES catalogs

The two families share every `physical`, `xram`, `port` and most `eeprom` and
`be` datapoints. They differ in:

- **Clock (`be`).** BEM `0xF4` weekday, `0xF5` hours and tens of minutes,
  `0xF6` minutes. BES `0x74` weekday, `0x75` hours, `0x76` minutes. VWMS and
  VBT2 have none. Both read `0xFF` on #35, so the clock cannot tell them
  apart.
- **Coding cells (`eeprom`).** The same setting sits in a different cell per
  panel: "Heizungsanlagenschema" (same value list) is BEM coding (00) at
  `0x08` and BES coding (04) at `0x04`.
  - BEM only: `0x08`, `0x0A`, `0x0D`, `0x13`, `0x14`, `0x1C`, `0x21`.
  - BES only: `0x03`.
  - Unused cells answer `FF FF`, so reading these cells shows which coding map
    the controller keeps. This is the open test for #35.

## Data encoding

- **Byte order: mostly little-endian, one exception suspected.**
  - The four timers decode consistently little-endian: for each,
    65536 − raw ≈ 562.5 × its duration in minutes (13, 7, 4 and 1). They never
    changed between reads, so they are most likely preset durations, not
    running timers (HW #35).
  - `xram` `0x88` "Wasserdruck" reads `01 72`. Little-endian gives 151 bar;
    big-endian gives 370 × 0.0052 − 0.65 = 1.27 bar against 1.2 bar on the
    gauge (HW #35, one reading). Vitosoft defines it as plain `Int`. A second
    reading at another pressure would settle it.
- **Bits** are numbered MSB-first in Vitosoft (§8e).
- **Absent sensors read `0xFF`** (255), for example "Raumtemperatur HK B"
  (`physical` `0x67`) on a single-circuit unit. #35's coding (04)
  Heizungsanlagenschema = 1 ("one heating circuit without mixer, with hot
  water") agrees.

## Hub behaviour built on these findings

- **Late-answer report.** After a timeout, any bytes that arrive before the
  next ENQ are logged once with their delay.
- **Poll backoff.** Three timeouts in a row while other datapoints answer
  slows that entity down: 2x, then up to 16x its interval. The first answer
  restores it.
- **All-`0xFF` warning.** Fires after 8 consecutive all-`0xFF` answers. In a
  full catalog the `be` reads are interleaved with real ones, so it does not
  fire there.
- **Known issue: link status flaps.** Three timeouts in a row mark the link
  offline. Consecutive silent datapoints (the fault history at `0xA0`, `0xAA`
  and `0xB4` is `kmbus_eeprom`) did that about 14 times in 6.5 minutes on
  #35, although the unit kept sending ENQs and answering other reads.

## Open questions

| Question | Test |
|---|---|
| Does #35's controller keep the BES or the BEM coding map? | Read `eeprom` `0x03`, `0x08`, `0x0A`, `0x0D`, `0x13`, `0x14`, `0x1C`, `0x21` with frames logged; compare BES codings (02), (03), (04) with the panel |
| Is `xram` `0x88` big-endian? | Second Wasserdruck reading at another pressure |
| Does any GWG unit serve `be`? | A unit with a BEM panel |
| Can `kmbus_eeprom` be reached at all? | Unknown; possibly an LGM29 setting |
| Where does the write frame's `0x04` go? | A first `eeprom` write on a unit whose owner accepts the risk |
