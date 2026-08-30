# Viessmann datapoint acronyms

Working glossary for reading Vitosoft-derived datapoint identifiers such as
`ADC_IstTemperaturwert_ATS~0x0800` or `TiefpassTemperaturwert_KTS~0x0810`.

**How these were derived.** The ViessData device lists pair a German UI label
with the datapoint token on the same line, e.g.

```
- Aussentemperatur (5373) [TiefpassTemperaturwert_ATS~0x5525 (SInt)]
```

Every expansion below is taken from the labels that actually co-occur with that
acronym across the 171 device lists, not from guessing at the letters. Where
the label evidence is thin or a token is ambiguous, that is stated rather than
smoothed over. Counts are occurrences of the acronym in the ~10 400 event ids
of `ecnEventType.xml`.

---

## Temperature sensors (`…TS` = **T**emperatur**S**ensor)

The suffix `TS` is consistently *TemperaturSensor*. The prefix names the
measuring point.

| Acronym | Expansion | Meaning | Observed labels | n |
| --- | --- | --- | --- | --- |
| `ATS` | **A**ußen**TS** | outside air | "Aussentemperatur", "Temperatur Sensor 1" | 19 |
| `KTS` | **K**essel**TS** | boiler water | "Kesseltemperatur", "Temperatur Sensor 3" | 13 |
| `STS1` | **S**peicher**TS** 1 | DHW cylinder, upper | "Warmwassertemperatur (STS1)", "Sensor 5" | — |
| `STS2` | **S**peicher**TS** 2 | DHW cylinder, lower | "Warmwassertemperatur (STS2)", "Sensor 5B" | — |
| `AGTS` | **A**b**G**as**TS** | flue gas | "Abgastemperatur", "Sensor 15" | 9 |
| `VTS` | **V**orlauf**TS** | common flow (cascade) | "Gem. Vorlauftemperatur" | 7 |
| `VLTS` | **V**or**L**auf**TS** | flow, explicit spelling | "Status Sensor 17B", "Status Sensor VTS" | 2 |
| `RLTS` | **R**ück**L**auf**TS** | return | "Rücklauftemperatur 17A/17B" | 6 |
| `RTS` | **R**aum**TS** | room | "Raumtemperatur A1M1 / M2 / M3" | 12 |
| `PTS` | **P**uffer**TS** | buffer cylinder | "Pufferspeicher-Temperatur", "Sensor 9" | 6 |
| `WWRLTS` | **W**arm**W**asser-**R**ück**L**auf**TS** | DHW return | "Ecotronic_Status_WWRLTS" | 1 |

Two things worth internalising:

- **`VTS` and `VLTS` are not cleanly distinct.** One list labels `VLTS` as
  "Status Sensor VTS". Treat both as *flow temperature* and rely on the address,
  not the acronym.
- **`RTS` is room, not return.** Return is `RLTS`. Getting these two the wrong
  way round is the easiest mistake in this whole table.

### Sensor numbers

Service documentation numbers the sensors, and the lists carry both forms:
Sensor 1 = `ATS`, 3 = `KTS`, 5 = `STS1`, 5B = `STS2`, 9 = `PTS`, 15 = `AGTS`,
17A = `RLTS`, 17B = `VLTS`. When a manual says "Sensor 17A", that is the return
sensor.

---

## Value-stage prefixes — *the important distinction*

The same physical sensor appears at several addresses in different processing
stages. This is the single most useful thing in this document, because picking
the wrong stage gives a plausible-looking but wrong reading.

| Prefix | Meaning | Behavior |
| --- | --- | --- |
| `ADC_IstTemperaturwert_…` | raw converter value | unfiltered, noisy, follows the sensor immediately |
| `TiefpassTemperaturwert_…` | low-pass filtered | smoothed; what the controller regulates on and shows on the panel |
| `NRF_Tiefpass…` | low-pass, NRF controller family | as above, different controller generation |
| `TemperaturFehler_…` | sensor fault flag | not a temperature — a status/error bit |
| `…_Soll` / `…soll` | setpoint | commanded, not measured |
| `…_eff` | effective | the value actually in force after all overrides |

Concretely, outside temperature exists as **raw ADC** at `0x0800` and as
**low-pass** at `0x5525`. The low-pass one is what the heating curve uses.

---

## Address blocks

Observed layout, 2 bytes per entry, even addresses:

```
0x0800  ADC ATS      0x0808  ADC AGTS
0x0802  ADC KTS      0x080A  ADC 17A (RLTS)
0x0804  ADC STS1     0x080C  ADC 17B (VLTS)
0x0806  ADC STS2     0x080E  NRF low-pass ATS
```

```
0x0810  low-pass KTS     0x0816  low-pass AGTS
0x0812  low-pass WW1     0x081A  low-pass VTS
0x0814  low-pass WW2     0x5525  low-pass ATS
```

**Caution:** these blocks are *not* identical across device families. On
`VScotHO1_72`, `0x080C` is `Temperatur_Hydraulische_Weiche`, not `ADC 17B`.
Same address, different sensor. Verify against your own device list.

---

## Heating circuits and hydraulics

| Acronym | Expansion | Notes | n |
| --- | --- | --- | --- |
| `HK` | **H**eiz**K**reis | heating circuit | 175 |
| `HK1`/`A1M1`, `HK2`/`M2`, `HK3`/`M3` | circuits 1–3 | `A1M1` = circuit 1 (unmixed), `M2`/`M3` mixed | — |
| `WW` | **W**arm**W**asser | domestic hot water | 161 |
| `WW1` / `WW2` | DHW sensors 1 / 2 | upper / lower cylinder | 130 / 131 |
| `HKP` | **H**eiz**K**reis**P**umpe | circulation pump | — |
| `Hydraulische Weiche` | low-loss header | hydraulic separator | — |
| `HYS` | **Hys**terese | switching hysteresis | 96 |

`A1M1` is worth remembering: it is the *first* heating circuit, and appears in
labels far more often than `HK1`.

---

## Burner and combustion

| Term | Meaning |
| --- | --- |
| `Brennerstarts` | burner start count (monotonic counter) |
| `Brennerlaufzeit` / `Betriebsstunden` | burner run hours (monotonic) |
| `Brennerleistung` / `Modulation` | current output, percent |
| `Flamme` | flame present |
| `Codierstecker` | coding plug — hardware configuration jumper |
| `Anlagenschema` | hydraulic system layout code |

Both counters are **monotonic by definition**. A decreasing value means the
address, the framing or the access mode is wrong — this is exactly how the GWG
`0x17` problem was spotted.

---

## Controller families and protocol prefixes

| Prefix | Meaning | n |
| --- | --- | --- |
| `GWG` | **G**as**W**and**G**erät — wall-mounted gas unit, oldest protocol | 490 |
| `WPR` / `WPR3` | **W**ärme**P**umpen**R**egelung — heat-pump controller | 1161 / 1095 |
| `NRF` / `NRP` | controller generations (Vitotronic families) | 289 / 308 |
| `KBUS` | internal K-Bus accessory transport | 1707 |
| `KM` | **K**M-**B**US accessory bus | 43 |
| `LON` | LON fieldbus | — |
| `OT` | OpenTherm | — |
| `RPC` | Remote Procedure Call datapoint | 434 |
| `BEM` / `BE` | **BE**dienmodul — control panel | — |
| `FA` | **F**euerungs**A**utomat — burner control unit | 54 |
| `VK` / `VSKO` / `HV` / `FCU` | device/project families | — |

`WPR*` prefixes together outnumber everything else. If a token starts `WPR`, it
belongs to a heat pump and is irrelevant on a boiler.

---

## Units and data-type markers

| Marker | Meaning |
| --- | --- |
| `SInt` | signed integer — negative values legal (outside temperature!) |
| `Int` | unsigned integer |
| `Int4` | 4-byte integer, typically a counter |
| `Byte` | single byte, often an enum or flag |
| `Div10` / `Div2` | divide raw value by 10 / 2 |
| `UTI` | vcontrold unit: temperature, signed, ÷10 |

`SInt` versus `Int` matters most on outside temperature: read unsigned, −5 °C
becomes about 6 553 °C.

---

## German terms that recur in labels

| German | English |
| --- | --- |
| Ist / Soll | actual / target |
| Vorlauf / Rücklauf | flow / return |
| Kessel | boiler |
| Speicher | storage cylinder |
| Aussen | outside |
| Betriebsart | operating mode |
| Betriebsstunden | operating hours |
| Neigung / Niveau | heating-curve slope / offset |
| Drehzahl | speed (pump/fan) |
| Störung | fault |
| Freigabe / Sperren | enable / block |
| Anforderung | demand |
| Umschaltventil | diverter valve |
| Zirkulationspumpe | DHW circulation pump |
| Ladepumpe | cylinder charging pump |
| Sammelstörung | common fault relay |
| Tiefpass | low-pass (filter) |
| Wandlerwert | converter (ADC) value |
| Schaltzeiten | switching times / schedule |
| Einmalladung | one-off DHW charge |

---

## Reading a token

```
ADC_IstTemperaturwert_ATS~0x0800 (SInt)
│   │                    │  │      └─ signed
│   │                    │  └──────── address
│   │                    └─────────── Außentemperatursensor
│   └──────────────────────────────── "actual temperature value"
└──────────────────────────────────── raw ADC stage (unfiltered)
```

→ raw, unfiltered, signed outside temperature at `0x0800`, ÷10.

Compare `TiefpassTemperaturwert_ATS~0x5525`: same sensor, same units,
**low-pass filtered** — the value the controller actually regulates on.

---

## Caveats

- Derived from co-occurring German UI labels, not from Viessmann
  documentation. Expansions are well-evidenced but not authoritative.
- Acronyms are **not globally unique**. `VTS`/`VLTS` overlap; `0x1070` carries
  56 different token names across device families.
- A name is not proof of behavior. Verify a datapoint by watching it move
  sensibly before trusting it — and certainly before writing to it.
