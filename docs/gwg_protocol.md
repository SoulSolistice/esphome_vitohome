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
| Response | LEN raw bytes; `eeprom` a pair, `be` and `port` one byte, per address whatever LEN asks; no framing, checksum, address echo or error telegram | HW |
| Answer time | 23–53 ms after the request | HW #21, #35 |
| Answer deadline | 250 ms default (`set_gwg_response_timeout_ms()`, 50–2000 ms from a lambda) | design choice |
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
- **A burst must leave within about 100 ms of the previous answer** (HW #35,
  40 h log, 2026-11-19/20): answered almost always under 80 ms, mostly
  unanswered from 100 ms. The engine bursts for 80 ms (`ENQ_VALIDITY_MS`).
  After a late burst the fresh-ENQ retry failed 35 times out of 44: 9 of 9
  sent within 25 ms of that ENQ were answered, 35 of 35 sent 27-57 ms after
  it were not. Ordinary ENQ-paced reads answer at any of those delays, so the
  late frame seems to leave the controller less patient for one telegram.

## Access modes

Each mode is its own address space; the same address in two modes is two
unrelated registers. The catalog's `access:` comes from Vitosoft's `FCRead`.

| `access:` | Read | Write | Status on hardware |
|---|---|---|---|
| `physical` | `0xCB` | `0xC8` | answers, plausible values (HW #21, #35) |
| `virtual` | `0xC7` | `0xC4` | answers; identification lives here (HW #13, #21, #35) |
| `eeprom` | `0xAE` | `0xAD` | answers in complement pairs (HW #21, #35) |
| `xram` | `0xC5` | `0xC3` | answers (HW #35) |
| `port` | `0x6E` | `0x6D` | answers one byte per address whatever LEN asks; `0x00`–`0x03` in use (HW #35) |
| `be` | `0x9E` | `0x9D` | answers fast, one byte per address whatever LEN asks, but `0xFF` at every address tried (HW #21, #35) |
| `kmbus_eeprom` | `0x43` | none | never answers, in seven framings, not even late (HW #35); the frame is unsettled, see below |
| `kmbus_ram` | `0x33` | none | no Vitosoft datapoint uses it; `01 33 ADDR LEN 04` never answers (HW #35) |

### `eeprom`

- One telegram per address. The unit answers a pair: byte 0 is the one's
  complement of byte 1, byte 1 is the value (`b0 + b1 == 0xFF`).
- `FF FF` means "address not in use". A pair that does not add up to `0xFF`
  fails the read.
- The owner of #35 reports that half of `0x00..0xFF` answers `FF FF` and that
  the used part repeats from `0x80` (a 128-byte part). Vitosoft's GWG
  datapoints use `0x01..0x24` only.

### `be` and `port`

- Both answer one byte per address, whatever LEN the telegram asks for
  (HW #35, SW `0x1E`). The engine reads them like `eeprom`: one LEN-1
  telegram per address, sent as a burst, delivered together. Every catalog
  datapoint in these modes is one byte, so this only changes longer raw
  reads, which timed out before.

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
- vitohome sends the generic frame `01 43 ADDR LEN 04`. No source shows a
  working KM-bus read over GWG, so that frame is a guess:
  - vcontrold's `getkmaddr` command calls `GETKMDDR`, a macro that does not
    exist. Its `getkmtestaddr`, like the test command of every other mode,
    sends the generic frame with LEN `01`. vogod maps `KMBUS_EEPROM_READ` to
    no operation. The other Optolink projects (sarnau, philippoo66, idstein)
    only copy Vitosoft's function-code list.
  - The `01 33 10 00 04` in #35 was deduced by its reporter, not captured.
- The bus itself, with sources: [kmbus.md](kmbus.md).
- The openv wiki ("KM-Bus") rests on bus captures and on the firmware of a
  Vitotronic 200KW2, a Vitotrol 200A/300 and a 0-10 V function extension.
  It likens the KM-bus to the M-Bus in its electrical layer and character
  format (master modulates voltage, participants current, 1200 baud 8E1);
  its telegrams are Viessmann's own. Vitosoft's `Virtual_MBUS` reads are the
  meter bus, a different thing. All of this is the bus's own wire between
  the controller and its participants, and the controller is always its
  master: participants only answer it. vitohome never sees that wire. It
  reaches the bus only through the controller, over Optolink at 4800 baud
  8E2, so a KM-bus read is a GWG request the controller carries out. What
  carries over is the telegram and the bus's speed.
- Telegrams are `DK SK CMD LEN DSL SSK data CRC-lo CRC-hi`: destination
  class, source class, command, the whole telegram's length, destination
  slot, source subclass, and a CRC-16/KERMIT over everything before it. Every
  master telegram on the page has SK `00` and SSK `01`. `31` reads one byte
  and `33` N bytes, answered by `B1` and `B3`; a `B3` answer puts each byte's
  address before it (`ADR0 DAT0 ADR1 DAT1 ...`). `43` and `C3` are in the
  200KW2's command table, but neither the Vitotrol nor the function extension
  uses them.
- Each participant has a 4-byte ID (class, device type, two serial bytes) at
  `F8..FB`, and the 200KW2 reads it from every class it knows about every five
  minutes. The read for class `01` slot `01` is
  `01 00 33 0A 01 01 F8 04 31 B4`.
- At 1200 baud a byte (11 bits) takes 9.2 ms. A one-byte `33` read and its
  answer take 205 ms on the bus, the ID read 260 ms (captured, kmbus.md
  "Timing"). An answer the
  controller relays from the bus therefore cannot come back in the 23-53 ms
  GWG answers take, and hardly within the 250 ms deadline. An answer from the
  controller's own memory can. After the deadline, bytes show in the
  late-answer report until the next telegram goes out, and
  `set_gwg_response_timeout_ms()` raises the deadline. On #35 the generic
  `kmbus_eeprom` reads had no late bytes either.
- The GWG TYPE bytes of the two KM-bus modes are bus commands: `kmbus_ram`
  `0x33` is the `33` read and `kmbus_eeprom` `0x43` is the unexplained `43`.
  A GWG read's `ADDR LEN` is also exactly a `33` telegram's data, `ADR N`.
  That suggests the controller builds the bus telegram from the GWG frame
  itself. Nothing confirms this yet.
- Vitosoft gives a `PrefixRead` only to function codes that reach a device
  behind the controller: KM-bus, K-bus (`KBUS_*`), Wilo pumps, M-Bus
  meters, a market manager and remote procedure calls. There the prefix
  appears to select the device or record, and the gateway does the bus
  traffic. Of the 91 `KMBUS_EEPROM_READ` datapoints, the 90 of the burner
  control (`GWG_FA_*`, `GWG_FA27_*`) all carry `03 00 00 00 01 01`; one
  (`KBUS_Schalterblock`) has none. No datapoint uses `KMBUS_RAM_READ`.
- Read as a telegram header, that prefix is DK `03`, SK `00`, CMD `00`, LEN
  `00`, DSL `01` and SSK `01`. SK and SSK are the master's own fields, and
  CMD and LEN would be the controller's to fill from the TYPE byte and the
  length, though CMD `00` is also the bus's PING. So the prefix plausibly
  names the participant, which the generic frame cannot. Where Vitosoft puts
  it in the GWG frame is unknown. No capture shows class `03`, but none is
  from a GWG controller. The `03` #35's BES shows is a participant code
  (burner control unit and a speed-controlled pump), not a bus class.
- To test that without firmware changes, the raw lane takes an open frame
  (raw_op.h): `gwg f <bytes...> [/ <n>]` sends the bytes exactly as given and
  collects the answer, and `[ ... ]` appends the CRC. Candidates, each for
  address `01`, one byte:

  | Frame | Idea |
  |---|---|
  | `01 43 01 01 04` | the generic frame (what the catalog sends) |
  | `01 33 01 01 04` | the generic `kmbus_ram` frame, TYPE `33` the bus's read |
  | `01 43 03 00 00 00 01 01 01 01 04` | the prefix naming the participant, then address and length |
  | `01 43 03 00 00 00 01 01 01 04` | the prefix before the address, no length byte (#35's reporter, as given) |
  | `01 43 [03 00 00 00 01 01 01] 04` | the same with a CRC (#35's reporter) |
  | `01 43 [03 00 43 0A 01 01 01 01] 04` | a whole telegram, CRC `E2 86`; needs a controller that relays bytes unchanged |
  | `01 43 [03 00 33 0A 01 01 01 01] 04` | the same with the bus's read command, CRC `E3 43` |

  `example/vitohome-gwg-kmbus-probe.yaml` sends these and the same seven for
  the ID at `F8`, with a 600 ms answer deadline for answers relayed over the
  bus. On #35 (SW `0x1E`) none of the 14 was answered, not even late (HW,
  2026-11-16).

### Writes

Vitosoft pairs every writable datapoint within one mode (`EEPROM_WRITE` 1082,
`BE_WRITE` 280, `PHYSICAL_WRITE` 0). Given the `be` results above, `eeprom`
datapoints are the candidates left, and those are codings.

**EEPROM writes work** (HW #35, the #13 unit: `0x2053` HW `0x01` SW `0x2B`,
2026-11-13). An EEPROM write is `01 AD ADDR ~V V 04`, one cell per frame
(design_notes.md, "GWG EEPROM"). The unit stores the two bytes after the
address as the cell's complement pair:

| Sent | Answer | Read back `0x16` / `0x96` |
|---|---|---|
| `01 AD 16 CF 30 04` | `00` | `CF:30` / `CF:30`; `CF:30` again 16 min later |
| `01 AD 16 D7 28 04` | `00` | `D7:28` / `D7:28` |

- `0x96` follows the write: `0x80`–`0xBF` is the same cell, not a copy.
- The generic layout put the length byte after the address, so the cell read
  back `01:29` or `01:04` and failed its check (HW #35, SW `0x1E` and
  `0x2B`).
- Not shown yet: that a value survives a power cycle, and that the
  controller acts on it.

No other write type is confirmed. `be` answers `0xFF` everywhere on the
units tried, and for the non-EEPROM types where the `0x04` goes is unknown;
both layouts are switchable at run time
(`set_gwg_write_eot_before_payload()`).

A raw EEPROM write of two bytes, `queue_raw_op("gwg w AD ADDR B0 B1")`, sends
them as the cell's pair unchanged (`01 AD ADDR B0 B1 04`), for example `FF FF`
to mark a cell unused. Nothing checks the pair, so it can store one that
every read rejects; it needs `unsafe_raw_writes: true`.

The engine completes a write on any single answer byte. The confirmed EEPROM
writes answered `00` (as on KW); whether other values occur is unknown, so
the byte is not checked but shown: the
raw result reads `write ACK (1 byte), answered XX`, and an entity write logs
it at DEBUG. Only a read-back proves that a write landed.

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
  - `xram` `0x88` "Wasserdruck" is big-endian although Vitosoft defines it as
    plain `Int`: `01 72` is 370 × 0.0052 − 0.65 = 1.27 bar against 1.2 bar on
    the gauge, little-endian it would be 151 bar. Over two days it read
    `01 71`..`01 7C`, 1.27..1.33 bar, rising with the boiler temperature (HW
    #35). The catalogs apply it as a hardware finding
    (`scripts/vitocatalog/confirmed.toml`) to every GWG device that has the
    row: they all share it.
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
- **Link status.** Three timeouts in a row mark the link offline, but only
  timeouts of a request that never went out because no ENQ came
  (`GWGEngine::lastTimeoutWasUnsynced()`). A request the unit took and did
  not answer is a datapoint it does not serve and leaves the link state
  alone. Before this, consecutive silent datapoints (the fault history at
  `0xA0`, `0xAA` and `0xB4` is `kmbus_eeprom`) published the link offline
  about 14 times in 6.5 minutes on #35, although the unit kept sending ENQs
  and answering other reads. Not yet confirmed on hardware.
- **ENQ-to-send delay (diagnostic, off by default).** A request goes out as
  soon as the ENQ is read. `id(vito).set_gwg_enq_send_delay_ms(30)` from a
  lambda (0 to 100 ms) holds an ENQ-paced request that long first; burst-sent
  requests do not wait. The DEBUG `GWG timing:` line shows `enq age` and the
  measured `enq->send` for each answered read, and `GWG 0x.. unanswered:`
  shows the same two for each unanswered one. `enq->send` runs to the write
  that sends the frame's first byte; the frame log writes its lines after
  that write. On #35, reads sent 25 ms or more after the ENQ went unanswered
  4 times in 439, all identification reads right after an ESP boot; reads
  sent within 10 ms went unanswered 236 times in 1,882, in stretches (HW,
  2026-11-13 to 2026-11-17). The two settings ran at different times, not
  alternating.
- **Gap after the `0x01` (diagnostic, off by default).** The delay above moves
  the whole frame, its `0x01` included. `id(vito).set_gwg_enq_ack_gap_ms(30)`
  from a lambda (0 to 100 ms) sends the `0x01` on the ENQ, or after the delay,
  and the rest of the frame that much later. Open frames that start with
  `0x01` get the gap too; burst-sent requests carry no `0x01` and do not wait.
  Both DEBUG lines then add the measured `01->rest`. #35's reporter's own
  firmware sends the `0x01` at once and the rest one or two loop passes
  later. On #35 a gap of 30 ms made no difference: alternating per read, 85
  of 550 reads went unanswered at 30 ms and 86 of 550 at 0 ms (HW,
  2026-11-17).

## Open questions

| Question | Test |
|---|---|
| Does #35's controller keep the BES or the BEM coding map? | Read `eeprom` `0x03`, `0x08`, `0x0A`, `0x0D`, `0x13`, `0x14`, `0x1C`, `0x21` with frames logged; compare BES codings (02), (03), (04) with the panel |
| Does any GWG unit serve `be`? | A unit with a BEM panel |
| How does a GWG unit frame a KM-bus read? | The candidates under `kmbus_eeprom`, with the KM-bus probe |
| Does #35's unit answer more reads with an ENQ-to-send delay? | A read every 5 s with frames logged, `set_gwg_enq_send_delay_ms(0)` and `30` alternating for 2 h, the first 20 min after boot not counted; compare the unanswered reads of each |
| Where does the write frame's `0x04` go? | A first `eeprom` write on a unit whose owner accepts the risk |
