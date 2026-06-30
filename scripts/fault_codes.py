"""Viessmann Vitotronic display-Stoerungscode (fault-code) maps.

Single source of truth for the `codes:` map that `gen_catalog.py` attaches to
`error_history` entities. Each map is `{int_code: text}` over the DISPLAY-code
space, i.e. byte[0] of an error-history slot. Stdlib-only, no dependencies.

SCOPE / SAFETY
- These map the Vitotronic byte-code space only. They do NOT apply to the GFA
  fault byte (0x5738), the LON alarm record, EEPROM/I2C status, or the
  self-describing sensor-status enums.
- VD100 / VD300 use a different multi-byte numeric (E3) scheme that is NOT
  byte-compatible with the register byte, so they are deliberately NOT folded in
  here -- a byte->text map cannot represent them correctly.
- Fault-code SEMANTICS are device-variant-specific. Pick the set that matches
  your unit and verify on the hardware (see CONFLICTS).

SETS
- OPENV            -- openv / community generic map (cross-checked vs vcontrold).
- VITOTRONIC_VD200 -- the Viessmann Vitodens 200 (WB2A) Serviceanleitung
                      Stoerungscode table (59 codes). ASCII-folded wording.
- VITOTRONIC_VD300_B3HA -- the Viessmann Vitodens 300-W (type B3HA)
                      Serviceanleitung table (94 codes), faithful German
                      (umlauts kept). This is the AUTHORITATIVE set for
                      VScotHO1_72 ("Projekt Neptun"), the Vitotronic 200
                      controller in the Vitodens 300-W B3HA -- the default.
                      OCR caveats: 0x89/0x95/0x96/0x97 were blank in the export
                      and 0x91/0x99 have a garbled sensor position; verify those
                      against the PDF.
- UNION            -- VD300-W extended with the codes VD200 then OPENV add that
                      it lacks (most-specific manual wins). Maximum coverage,
                      mixed provenance/encoding.

CONFLICTS holds the codes where OPENV and VD200 genuinely DISAGREE on meaning
(not just wording): value -> (openv_text, vd200_text). Neither is guaranteed for
a unit that is neither openv-generic nor a VD200 WB2A (e.g. VScotHO1_72).
"""

OPENV = {
    0x00: "Regelbetrieb (kein Fehler)",
    0x0F: "Wartung (fuer Reset Codieradresse 24 auf 0 stellen)",
    0x10: "Kurzschluss Aussentemperatursensor",
    0x18: "Unterbrechung Aussentemperatursensor",
    0x19: "Unterbrechung Kommunikation Aussentemperatursensor (Funk)",
    0x20: "Kurzschluss Vorlauftemperatursensor",
    0x21: "Kurzschluss Ruecklauftemperatursensor",
    0x28: "Unterbrechung Vorlauftemperatursensor",
    0x29: "Unterbrechung Ruecklauftemperatursensor",
    0x30: "Kurzschluss Kesseltemperatursensor",
    0x38: "Unterbrechung Kesseltemperatursensor",
    0x40: "Kurzschluss Vorlauftemperatursensor M2",
    0x42: "Unterbrechung Vorlauftemperatursensor M2",
    0x50: "Kurzschluss Speichertemperatursensor",
    0x58: "Unterbrechung Speichertemperatursensor",
    0x92: "Solar: Kurzschluss Kollektortemperatursensor",
    0x93: "Solar: Kurzschluss Sensor S3",
    0x94: "Solar: Kurzschluss Speichertemperatursensor",
    0x9A: "Solar: Unterbrechung Kollektortemperatursensor",
    0x9B: "Solar: Unterbrechung Sensor S3",
    0x9C: "Solar: Unterbrechung Speichertemperatursensor",
    0x9E: "Solar: Zu geringer Ertrag / Durchfluss",
    0xB0: "Kurzschluss Abgastemperatursensor",
    0xB1: "Unterbrechung Abgastemperatursensor",
    0xBA: "Kommunikationsfehler Erweiterung Mischerkreis M2",
    0xBC: "Kommunikationsfehler Fernbedienung Vitotrol A1",
    0xBD: "Kommunikationsfehler Fernbedienung Vitotrol M2",
    0xBE: "Falsche Codierung Fernbedienung",
    0xC2: "Kommunikationsfehler Erweiterung extern (LON)",
    0xC5: "Kommunikationsfehler drehzahlgeregelte Pumpe",
    0xCD: "Kommunikationsfehler Vitocom",
    0xD1: "Brennerstoerung",
    0xDA: "Kurzschluss Raumtemperatursensor A1",
    0xDB: "Kurzschluss Raumtemperatursensor M2",
    0xE5: "Interner Fehler (Flammenverstaerker)",
    0xF0: "Interner Fehler (Regelung tauschen)",
    0xF1: "Abgastemperaturbegrenzer ausgeloest",
    0xF2: "Uebertemperatur",
    0xF4: "Flammensignal fehlt / Brenner stoert",
    0xFD: "Fehler Brennersteuergeraet / Codierung",
    0xFF: "Kommunikationsfehler Brennersteuergeraet",
}

VITOTRONIC_VD200 = {
    0x00: "Regelbetrieb (kein Fehler)",
    0x0F: "Wartung (Reset: Codieradresse 24 auf 0)",
    0x10: "Aussentemperatursensor Kurzschluss",
    0x18: "Aussentemperatursensor Unterbrechung",
    0x20: "Vorlauftemperatursensor Anlage/Weiche Kurzschluss",
    0x28: "Vorlauftemperatursensor Anlage/Weiche Unterbrechung",
    0x30: "Kesseltemperatursensor Kurzschluss",
    0x38: "Kesseltemperatursensor Unterbrechung",
    0x40: "Vorlauftemperatursensor Heizkreis M2 Kurzschluss",
    0x48: "Vorlauftemperatursensor Heizkreis M2 Unterbrechung",  # ADD
    0x50: "Speicher-/Komforttemperatursensor Kurzschluss",
    0x51: "Auslauftemperatursensor Kurzschluss",  # ADD
    0x58: "Speicher-/Komforttemperatursensor Unterbrechung",
    0x59: "Auslauftemperatursensor Unterbrechung",  # ADD
    0x92: "Solar: Kollektortemp.-sensor S1 (Vitosolic) Kurzschluss",
    0x93: "Solar: Speichertemp.-sensor S2 (Vitosolic) Kurzschluss",  # CONFLICT vs openv
    0x94: "Solar: Temperatursensor S3 (Vitosolic) Kurzschluss",  # CONFLICT vs openv
    0x9A: "Solar: Kollektortemp.-sensor S1 (Vitosolic) Unterbrechung",
    0x9B: "Solar: Speichertemp.-sensor S2 (Vitosolic) Unterbrechung",
    0x9C: "Solar: Temperatursensor S3 (Vitosolic) Unterbrechung",
    0x9F: "Fehler Solarregelung",  # ADD
    0xA7: "Bedienteil defekt",  # ADD
    0xB0: "Abgastemperatursensor Kurzschluss",
    0xB1: "Kommunikationsfehler Bedieneinheit (intern)",  # CONFLICT vs openv (Abgas-Unterbr.)
    0xB4: "Interner Fehler (Regelung austauschen)",  # ADD
    0xB5: "Interner Fehler (Regelung austauschen)",  # ADD
    0xB7: "Kesselcodierstecker fehlt/defekt/falsch",  # ADD
    0xB8: "Abgastemperatursensor Unterbrechung",  # ADD
    0xBA: "Kommunikationsfehler Erweiterungssatz Heizkreis M2",
    0xBC: "Kommunikationsfehler Fernbedienung Vitotrol A1",
    0xBD: "Kommunikationsfehler Fernbedienung Vitotrol M2",
    0xBE: "Falsche Codierung der Fernbedienung Vitotrol",
    0xBF: "Falsches Kommunikationsmodul LON",  # ADD
    0xC2: "Unterbrechung KM-BUS zur Solarregelung",
    0xC6: "Komm.-fehler drehzahlgeregelte ext. Heizkreispumpe M2",  # ADD
    0xC7: "Komm.-fehler drehzahlgeregelte ext. Heizkreispumpe A1",  # ADD
    0xCD: "Kommunikationsfehler Vitocom 100 (KM-BUS)",
    0xCE: "Kommunikationsfehler Externe Erweiterung",  # ADD
    0xCF: "Kommunikationsfehler Kommunikationsmodul LON",  # ADD
    0xDA: "Raumtemperatursensor Heizkreis A1 Kurzschluss",
    0xDB: "Raumtemperatursensor Heizkreis M2 Kurzschluss",
    0xDD: "Raumtemperatursensor Heizkreis A1 Unterbrechung",  # ADD
    0xDE: "Raumtemperatursensor Heizkreis M2 Unterbrechung",  # ADD
    0xE4: "Fehler Versorgungsspannung",  # ADD
    0xE5: "Fehler Flammenverstaerker",
    0xE6: "Abgas-/Zuluftsystem verstopft",  # ADD
    0xF0: "Interner Fehler (Regelung tauschen)",
    0xF1: "Abgastemperaturbegrenzer hat ausgeloest",
    0xF2: "Temperaturbegrenzer ausgeloest (Trockenlauf)",
    0xF3: "Flammensignal beim Brennerstart bereits vorhanden",  # ADD
    0xF4: "Flammensignal bei Brennerstart nicht vorhanden",
    0xF5: "Luftdruckwaechter bei Brennerstart nicht geoeffnet",  # ADD
    0xF7: "Luftdruckwaechter defekt",  # ADD
    0xF8: "Brennstoffventil schliesst verspaetet",  # ADD
    0xF9: "Geblaesedrehzahl beim Brennerstart zu niedrig",  # ADD
    0xFA: "Geblaesestillstand nicht erreicht",  # ADD
    0xFD: "Fehler Gasfeuerungsautomat",
    0xFE: "Starkes Stoerfeld (EMV) / Grundleiterplatte defekt",  # ADD
    0xFF: "Starkes Stoerfeld (EMV) oder interner Fehler",  # CONFLICT vs openv
}

# Viessmann Vitodens 300-W (type B3HA) Serviceanleitung Stoerungscode table,
# extracted from the manual. The authoritative set for VScotHO1_72 ("Projekt
# Neptun") = the Vitotronic 200 controller in the Vitodens 300-W B3HA (the flow
# sensor / Volumenstrom of this boiler is why codes 0x1D/0x1E/0x1F exist).
# 0x00 added as the standard no-fault label (the manual lists faults only).
# Gaps to fill from the PDF: 0x89/0x95/0x96/0x97 had blank cells in the export
#   (text not captured). 0x91/0x99 (garbled in the extract) were corrected via the
#   official viessmann.de fault-code page (see NOTICE.md).
VITOTRONIC_VD300_B3HA = {
    0x00: "Regelbetrieb (kein Fehler)",  # 0x00 added (no-fault); not in the manual's fault table
    0x10: "Kurzschluss Außentemperatursensor",
    0x18: "Unterbrechung Außentemperatursensor",
    0x19: "Unterbrechung Kommunikation Außentemperatursensor RF",
    0x1D: "Keine Kommunikation mit Sensor",
    0x1E: "Strömungssensor defekt",
    0x1F: "Strömungssensor defekt",
    0x20: "Kurzschluss Vorlauftemperatursensor Anlage",
    0x28: "Unterbrechung Vorlauftemperatursensor Anlage",
    0x30: "Kurzschluss Kesseltemperatursensor",
    0x38: "Unterbrechung Kesseltemperatursensor",
    0x40: "Kurzschluss Vorlauftemperatursensor Heizkreis 2 (mit Mischer)",
    0x44: "Kurzschluss Vorlauftemperatursensor Heizkreis 3 (mit Mischer)",
    0x48: "Unterbrechung Vorlauftemperatursensor Heizkreis 2 (mit Mischer)",
    0x49: "Codierung Erweiterung Mischer Heizkreis 2 falsch eingestellt",
    0x4C: "Unterbrechung Vorlauftemperatursensor Heizkreis 3 (mit Mischer)",
    0x4D: "Codierung Erweiterung Mischer Heizkreis 3 falsch eingestellt",
    0x50: "Kurzschluss Speichertemperatursensor",
    0x58: "Unterbrechung Speichertemperatursensor",
    0x90: "Kurzschluss Temperatursensor",
    0x91: "Kurzschluss Temperatursensor",  # OCR fix confirmed via viessmann.de fault-code page
    0x92: "Kurzschluss Kollektortemperatursensor",
    0x93: "Kurzschluss Speichertemperatursensor",
    0x94: "Kurzschluss Speichertemperatursensor",
    0x98: "Unterbrechung Temperatursensor",
    0x99: "Unterbrechung Temperatursensor",  # OCR fix confirmed via viessmann.de fault-code page
    0x9A: "Unterbrechung Kollektortemperatursensor",
    0x9B: "Unterbrechung Speichertemperatursensor",
    0x9C: "Unterbrechung Speichertemperatursensor",
    0x9E: "Zu geringer oder kein Volumenstrom im Solarkreis oder Temperaturwächter hat ausgelöst",
    0x9F: "Fehler Solarregelungsmodul oder Vitosolic",
    0xA2: "Anlagendruck zu niedrig",
    0xA3: "Abgastemperatursensor nicht richtig positioniert",
    0xA4: "Max. Anlagendruck überschritten",
    0xA7: "Bedienteil defekt",
    0xA8: "Luft in der internen Umwälzpumpe oder Mindest-Volumenstrom nicht erreicht",
    0xA9: "Interne Umwälzpumpe blockiert",
    0xB0: "Kurzschluss Abgastemperatursensor",
    0xB1: "Kommunikationsfehler Bedieneinheit",
    0xB5: "Interner Fehler",
    0xB7: "Fehler KesselCodierstecker",
    0xB8: "Unterbrechung Abgastemperatursensor",
    0xBA: "Kommunikationsfehler Erweiterungssatz für Heizkreis 2 (mit Mischer)",
    0xBB: "Kommunikationsfehler Erweiterungssatz für Heizkreis 3 (mit Mischer)",
    0xBC: "Kommunikationsfehler Fernbedienung Vitotrol Heizkreis 1 (ohne Mischer)",
    0xBD: "Kommunikationsfehler Fernbedienung Vitotrol Heizkreis 2 (mit Mischer)",
    0xBE: "Kommunikationsfehler Fernbedienung Vitotrol Heizkreis 3 (mit Mischer)",
    0xBF: "Falsches Kommunikationsmodul LON",
    0xC1: "Kommunikationsfehler Erweiterung EA1",
    0xC2: "Kommunikationsfehler Solarregelungsmodul oder Vitosolic",
    0xC3: "Kommunikationsfehler Erweiterung AM1",
    0xC4: "Kommunikationsfehler Erweiterung Open Therm",
    0xC5: "Kommunikationsfehler drehzahlgeregelte interne Pumpe",
    0xC6: "Kommunikationsfehler drehzahlgeregelte, externe Heizkreispumpe Heizkreis 2 (mit Mischer)",
    0xC7: "Kommunikationsfehler drehzahlgeregelte externe Heizkreispumpe Heizkreis 1 (ohne Mischer)",
    0xC8: "Kommunikationsfehler drehzahlgeregelte, externe Heizkreispumpe Heizkreis 3 (mit Mischer)",
    0xCD: "Kommunikationsfehler Vitocom 100 (KM-BUS)",
    0xCF: "Kommunikationsfehler Kommunikationsmodul LON",
    0xD6: "Eingang DE1 an Erweiterung EA1 meldet Störung",
    0xD7: "Eingang DE2 an Erweiterung EA1 meldet Störung",
    0xD8: "Störung Eingang DE3 an Erweiterung EA1",
    0xDA: "Kurzschluss Raumtemperatursensor Heizkreis 1 (ohne Mischer)",
    0xDB: "Kurzschluss Raumtemperatursensor Heizkreis 2 (mit Mischer)",
    0xDC: "Kurzschluss Raumtemperatursensor Heizkreis 3 (mit Mischer)",
    0xDD: "Unterbrechung Raumtemperatursensor Heizkreis 1 (ohne Mischer)",
    0xDE: "Unterbrechung Raumtemperatursensor Heizkreis 2 (mit Mischer)",
    0xDF: "Unterbrechung Raumtemperatursensor Heizkreis 3 (mit Mischer)",
    0xE0: "Fehler externer LON-Teilnehmer",
    0xE1: "Ionisationsstrom während der Kalibrierung zu hoch",
    0xE2: "Keine Kalibrierung wegen zu geringen Volumenstrom",
    0xE3: "Zu geringe Wärmeabnahme während der Kalibrierung Temperaturwächter hat ausgeschaltet.",
    0xE4: "Fehler Versorgungsspannung 24 V",
    0xE5: "Fehler Flammenverstärker",
    0xE6: "Anlagendruck zu niedrig",
    0xE7: "Ionisationsstrom während der Kalibrierung zu gering",
    0xE8: "Ionisationsstrom nicht im gültigen Bereich",
    0xEA: "Ionisationsstrom während der Kalibrierung nicht im gültigen Bereich "
    "(zu große Abweichung gegenüber dem Vorgängerwert)",
    0xEB: "Wiederholter Flammenverlust während der Kalibrierung",
    0xEC: "Parameterfehler während der Kalibrierung",
    0xED: "Interner Fehler",
    0xEE: "Flammensignal ist bei Brennerstart nicht vorhanden oder zu gering.",
    0xEF: "Flammenverlust direkt nach Flammenbildung (während der Sicherheitszeit).",
    0xF0: "Interner Fehler",
    0xF1: "Abgastemperaturbegrenzer hat ausgelöst.",
    0xF2: "Temperaturbegrenzer hat ausgelöst.",
    0xF3: "Flammensignal ist beim Brennerstart bereits vorhanden.",
    0xF7: "Kurzschluss oder Unterbrechung Wasserdrucksensor",
    0xF8: "Brennstoffventil schließt verspätet.",
    0xF9: "Gebläsedrehzahl beim Brennerstart zu niedrig",
    0xFA: "Gebläsestillstand nicht erreicht",
    0xFC: "Gaskombiregler defekt oder fehlerhafte Ansteuerung Modulationsventil oder Abgasweg versperrt",
    0xFD: "Kessel-Codierstecker fehlt / Fehler Feuerungsautomat",
    0xFE: "Kessel-Codierstecker oder Grundleiterplatte defekt oder falscher Kessel-Codierstecker",
    0xFF: "Interner Fehler oder Entriegelungstaste R blockiert",
}

CONFLICTS = {
    0x93: ("Solar: Kurzschluss Sensor S3", "Solar: Speichertemp.-sensor S2 Kurzschluss"),
    0x94: ("Solar: Kurzschluss Speichertemperatursensor", "Solar: Temperatursensor S3 Kurzschluss"),
    0xB1: ("Unterbrechung Abgastemperatursensor", "Komm.-fehler Bedieneinheit (VD200: Abgas-Unterbr. is 0xB8)"),
    0xFF: ("Kommunikationsfehler Brennersteuergeraet", "Starkes Stoerfeld (EMV) oder interner Fehler"),
}

# UNION: maximum coverage, most-specific manual wins. VD300-W (B3HA) is the most
# complete map and is the one that matches VScotHO1_72; VD200 fills the codes it
# lacks, OPENV fills the rest. Built here so it can never drift from the sources.
UNION = dict(VITOTRONIC_VD300_B3HA)
for _src in (VITOTRONIC_VD200, OPENV):
    for _code, _text in _src.items():
        UNION.setdefault(_code, _text)


# Name -> map. `gen_catalog.py --error-code-set` selects from these.
SETS = {
    "openv": OPENV,
    "vd200": VITOTRONIC_VD200,
    "vd300": VITOTRONIC_VD300_B3HA,
    "union": UNION,
}


def get(name: str) -> dict:
    """Return the fault-code map for a set name, or raise KeyError."""
    return SETS[name]
