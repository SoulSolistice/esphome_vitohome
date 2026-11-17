# KM-bus: field reference

The KM-bus is the wire between a Vitotronic controller and its bus
participants: remote controls, extensions, Vitocom, and on a Vitodens 200 WB2
the burner control unit. vitohome never touches this wire. It matters for the
GWG modes `kmbus_ram` and `kmbus_eeprom`, which ask the controller to read
from it ([gwg_protocol.md](gwg_protocol.md#kmbus_eeprom)).

Listed here is what a capture, a log or a firmware analysis shows. Inferences
say so.

## Sources

| Tag | Source | Content |
|---|---|---|
| W | openv wiki, pages "KM-Bus" and "KM-Bus-Interface" | bus captures, and the disassembled firmware of a Vitotronic 200KW2 (KW protocol, `0x2098`), a Vitotrol 200A/300 and a 0-10 V function extension |
| C | openv#387, `Capture_busKM_1h.zip` | 64 min of master telegrams on a Vitodens 333-F with a Vitotrol 300 and an EA1: 5,269 telegrams, every CRC valid |
| T | openv#387, `kmbus.zip` and `kmBusAgent.zip` (2018) | an emulated Vitotrol on a Vitocrossal 300, both directions, about 211,000 telegrams |
| V | openv#387, `log.zip` | the transmit line of a real Vitotrol 300 |
| I | boblegal31/Heater-remote, dumpfheimer/WiFiVitotrol, timob0's kmBusAgent | participant emulations that pair with real controllers |
| S | Vitosoft definitions | function codes, `PrefixRead` |

## Physical layer

- Two wires, powered by the controller, which is the master. The master
  sends by modulating the bus voltage, a participant by modulating its
  current, as on the M-Bus (W, I). A listener with an M-Bus slave
  transceiver hears only the master (W, C).
- 1200 baud, 8E1 (W, I). One byte takes 9.17 ms. C fits 11 bits per byte
  only: the idle time around a read stays at 21-24 ms whether the answer
  has 10, 12 or 16 bytes, while 12 bits per byte would leave 7, 3 and 0 ms.

## Telegram

`DK SK CMD LEN DSL SSK data CRC-lo CRC-hi`

| Field | Meaning |
|---|---|
| `DK`, `SK` | destination and source class; `00` is the master, `FF` broadcast |
| `CMD` | command, see below |
| `LEN` | the whole telegram, CRC included |
| `DSL` | the destination's slot within its class; `00` on broadcasts |
| `SSK` | `01` in every telegram of C, T and V |
| CRC | CRC-16/KERMIT over all bytes before it, low byte first |

- `LEN` counts the whole telegram in every telegram of C, T and V. The real
  Vitotrol's ID answer starts `00 11 B3 10` and is 16 bytes long. Where W
  gives other values (`0C` in one of its two ID answers, `09` for a master's
  `B1`), the captures contradict it: C's `B1` writes carry `0A`.
- The Vitotrol's slots `01`-`03` are heating circuits 1-3 (W, I).

## Commands

Bit 7 clear marks a request, bit 7 set carries data (W, I).

| Master sends | Participant answers | Meaning |
|---|---|---|
| `00` | `80`, or a `BF` record | ping; the participant may send |
| `31 ADR` | `B1 ADR DAT` | read one byte |
| `33 ADR N` | `B3 ADR0 DAT0 ADR1 DAT1 ...` | read N bytes |
| `3F` | `BF` or nothing | request a record |
| `B1 ADR DAT` | nothing | write one byte |
| `B3 ADR0 DAT0 ...` | nothing | write N bytes; the time broadcast to `FF` is one |
| `BF` | nothing | send a record |
| `43`, `C3` | ? | a pair in the 200KW2's command table; neither the Vitotrol nor the function extension supports it (W) |

- Participants never send a request. No telegram in W, C, T or V has one;
  V holds 416 pongs, 30 records and the two answers of pairing. The
  controller also ignored a `3F` from an emulated Vitotrol (T's notes). A
  participant gets the controller's data only through records.
- Writes go unanswered. In C the master's next telegram starts 15-22 ms
  after a `B1`, `B3` or `BF` ends, too soon for an 8-byte pong (73 ms).
  WiFiVitotrol answers writes with a pong anyway.
- Record data, the bytes after the record number, are XORed with `AA`. The
  data of `31`, `33`, `B1` and `B3` are not (W, C, T, I).

## Participants

- A participant's addresses are one byte wide. `F8`-`FB` hold its ID:
  class, device type, two serial bytes (W, V, I). The real Vitotrol 300
  answers `11 38 01 0A`. Device type `34` is a Vitotrol 200A, `38` a
  Vitotrol 300 (W).
- The controller ignores an ID whose device type it does not accept for the
  class: a Vitocal 200-S stayed silent to type `38` and paired with `34`
  (openv#387).
- A Vitotrol answers `12` at address `00` (V), and the controller reads that
  address after pairing (T).
- Classes on the wire: `01`, `04`, `07`, `10`, `11`, `20` and `FF` (C, T).
  `11` is the Vitotrol, `04` the function extension (W). Class `03` appears
  in no source.
- Class `01` slot `01` on the Vitodens 333-F is a real participant of
  unknown kind. The master reads it every 9.9 s at `00`, `10`, `14`-`17`,
  `22`-`23`, `2F`, `32`-`33` and `3B`, and writes `01` and `10` (C).
- Class `20` slot `EE` answers with nothing connected (W) and has `10` and
  `11` written every 60 s (C).
- The 200KW2's class table (W): `11` slots `00`-`03`, `20` slots `02`,
  `03` and `EE`, `01` slots `01`-`03`, `04` slot `01`.

## Timing

All from C:

- A read and its answer take 205 ms for one byte (`31` or `33`) and 260 ms
  for four, the idle time included.
- A participant must start its answer within about 35-40 ms. After an
  unanswered ID read, the master's next telegram starts 37 ms later.
  Emulators found 30-40 ms too slow (openv#387).
- Absent participants are asked for their ID every 5 min (298.6 s), free
  Vitotrol slots every 60 s. A paired Vitotrol is pinged instead, about
  every 7 s. The time broadcast to `FF` goes out every 10 min.

## What this means for GWG

- The GWG TYPE bytes `33` (`kmbus_ram`) and `43` (`kmbus_eeprom`) equal the
  bus commands `33` and `43` (S, W). Inference from Vitosoft's names: `33`
  reads a participant's RAM-like address space and `43` its EEPROM.
- A relayed read would take 0.2-0.26 s on the bus. If the controller is in
  the middle of one of its own reads, add up to another 0.26 s. That is
  about 0.5 s at worst. The probe's 600 ms deadline covers it, and the
  late-answer report (2.5 s) covers far more.
- Vitosoft's `PrefixRead` `03 00 00 00 01 01` on the burner control unit's
  datapoints reads as DK `03`, SK `00`, empty CMD and LEN, DSL `01`, SSK
  `01`. SK and SSK are those of every master telegram. No source shows
  class `03`, but no source is a GWG controller.
- The `03` a Vitodens 200 WB2's BES shows is a participant code from the
  service manual, not a bus class: `02` burner control unit, plus `01` for
  a speed-controlled pump, plus `04` for a mixer extension, plus `10` for a
  Vitocom 100 (#35). That list counts the burner control unit as a KM-bus
  participant.
- KW has no KM-bus TYPE. A KW controller keeps the data it polls from its
  participants in its own memory, which Optolink reads directly (W). So the
  200KW2's firmware says nothing for or against a GWG controller relaying a
  read to the bus.
- Not established: whether any GWG controller puts a TYPE `33` or `43` frame
  on the bus, and in which framing. The #35 unit (SW `0x1E`) answered none
  of the 14 framings of `example/vitohome-gwg-kmbus-probe.yaml`, not even
  late (HW, 2026-11-16).
