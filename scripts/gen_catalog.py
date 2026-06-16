#!/usr/bin/env python3
"""Generate a vitohome ESPHome package from a Viessmann Vitosoft XML export.

Per-device datapoint definitions (address, length, conversion, access type,
enums, units, borders) are NOT discoverable over the Optolink bus -- they come
from Viessmann's own Vitosoft data, surfaced by the InsideViessmannVitosoft
scripts. This script reads that export and emits a ready-to-include ESPHome
*package* with each datapoint on the right platform (sensor / binary_sensor /
number / select / text_sensor), the correct ``converter:``, real ``length:``,
bit masks, units and a sensible poll interval.

The 2026 export (verified against the real ~212 MB file) splits the data across
several files; this generator reads all of them:

  DPDefinitions.xml      .NET DataSet diffgram. Tables (repeated elements):
                         ecnDatapointType (device types; Address = token),
                         ecnDataPointTypeEventTypeLink (DataPointTypeId ->
                         EventTypeId), ecnEventType (Name, Address-*token*,
                         Conversion, Type, EnumType), ecnEventTypeEventValueType
                         Link (EventTypeId -> *EventValueId*), ecnEventValueType
                         (enum values + Unit + borders).
                         NOTE: the diffgram declares
                         xmlns="http://tempuri.org/ECNDataSet.xsd", so every
                         element is namespaced -- we match on LOCAL tag names.
  ecnEventType.xml       ACCESS layer: per event token (its <ID>), the real
                         Optolink <Address> 0xNNNN, BlockLength/ByteLength,
                         BitLength/BitPosition/BytePosition, FCRead/FCWrite,
                         ConversionFactor, Unit. DPDefinitions does NOT carry
                         lengths in 2026 -- they live here and are joined on the
                         event's Address token.
  ecnDataPointType.xml   IDENTIFICATION: per datapoint-type token, Identification
                         (group 0xF8 << 8 | ident 0xF9) and IdentificationExt..
                         Till (HW 0xFA << 8 | SW 0xFB). Used by --identify to
                         pick the correct software revision automatically.
  Textresource.xml       UTF-16; <TextResource Label=.. Value=.. CultureId=..>.
                         Resolves @@-prefixed names/enum labels. (Optional.)

Typical use::

    # what device tables (and identification ranges) does this export contain?
    python3 scripts/gen_catalog.py --data <export-dir> --list-devices

    # pick the revision automatically from the bytes the hub already logs:
    #   Device: 0x20CB (VScotHO1) HW=0x03 SW=0x51
    python3 scripts/gen_catalog.py --data <export-dir> \
        --identify 0x20CB --hw 0x03 --sw 0x51 \
        --profile standard --out my-heater.vitohome.yaml

    # ...or name the datapoint-type token directly
    python3 scripts/gen_catalog.py --data <export-dir> --device VScotHO1_72 ...

Include the emitted file from your device YAML::

    packages:
      heater: !include my-heater.vitohome.yaml

and define the ``vitohome:`` hub (and ``uart:``) yourself. Every entity is
``disabled_by_default: true`` so you opt in from Home Assistant. Always run
``esphome config`` then ``esphome compile``/``run`` before relying on a value --
not every address answers on every firmware.

stdlib only; runs in CI and in a bare Python install.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict
from dataclasses import dataclass, field

# --- conversion -> vitohome converter --------------------------------------
# Maps the Vitosoft ``Conversion`` name to a vitohome ``converter:`` plus a
# "kind" used for platform/poll decisions. None converter means "numeric but no
# scaling preset" -> emit as noconv (raw) with a NOTE.
DIV = "div"
COUNTER = "counter"
PLAIN = "plain"

CONVERSION_MAP = {
    "": ("noconv", PLAIN),
    "NoConversion": ("noconv", PLAIN),
    "Div2": ("div2", DIV),
    "Div10": ("div10", DIV),
    "Div100": ("div100", DIV),
    "Div1000": ("div1000", DIV),
    "Mult2": ("mult2", PLAIN),
    "Mult5": ("mult5", PLAIN),
    "Mult10": ("mult10", PLAIN),
    "Mult100": ("mult100", PLAIN),
    "Sec2Hour": ("sec2hour", COUNTER),
    "Sec2Minute": (None, COUNTER),  # no preset; emit noconv + note
}

# Conversions that are not a plain scaled integer; we cannot auto-emit a numeric
# sensor for these, so they are surfaced as commented hints in the output.
NON_NUMERIC_CONVERSIONS = {
    "DateTimeBCD": "8-byte date/time -> error_history or custom decode",
    "DateBCD": "date -> custom decode",
    "Time53": "switching times -> custom decode",
    "IPAddress": "string -> text_sensor + custom decode",
    "Phone2BCD": "string -> custom decode",
    "Convert4BytesToFloat": "IEEE-754 float -> not yet supported by vitohome",
    "HexToFloat": "IEEE-754 float -> not yet supported by vitohome",
    "RotateBytes": "byte array -> custom decode",
    "MultOffsetFloat": "float -> custom decode",
}

# Best-effort unit symbols for Viessmann's ecnUnit.* tokens (Textresource.xml
# does not carry these). Unknown ecnUnit.* tokens fall back to the bare suffix.
ECN_UNITS = {
    "ecnUnit.Grad C": "°C",
    "ecnUnit.Kelvin": "K",
    "ecnUnit.KelvinProStunde": "K/h",
    "ecnUnit.Prozent": "%",
    "ecnUnit.ProzentProStunde": "%/h",
    "ecnUnit.Stunden": "h",
    "ecnUnit.Minuten": "min",
    "ecnUnit.Sekunden": "s",
    "ecnUnit.Tage": "d",
    "ecnUnit.Monate": "months",
    "ecnUnit.Jahre": "a",
    "ecnUnit.Bar": "bar",
    "ecnUnit.MilliBar": "mbar",
    "ecnUnit.Pascal": "Pa",
    "ecnUnit.HektoPascal": "hPa",
    "ecnUnit.Watt": "W",
    "ecnUnit.KiloWatt": "kW",
    "ecnUnit.KiloWattStunden": "kWh",
    "ecnUnit.KiloWattstunde": "kWh",
    "ecnUnit.MegaWattStunden": "MWh",
    "ecnUnit.Liter": "L",
    "ecnUnit.LiterProStunde": "L/h",
    "ecnUnit.LiterProMinute": "L/min",
    "ecnUnit.KubikMeter": "m³",
    "ecnUnit.KubikMeterProStunde": "m³/h",
    "ecnUnit.Ampere": "A",
    "ecnUnit.MilliAmpere": "mA",
    "ecnUnit.Volt": "V",
    "ecnUnit.Hertz": "Hz",
    "ecnUnit.Umdrehungen": "rpm",
    "ecnUnit.UmdrehungenProMinute": "rpm",
    "ecnUnit.Grad": "°",
}

# Poll tiers (seconds).
POLL_FAST = 60  # live temperatures / measurements
POLL_SLOW = 600  # monotonic counters (hours, starts, consumption)
POLL_CODING = 3600  # writable coding values / setpoints (rarely change)
POLL_ERROR = 300  # error history

PROFILES = ("minimal", "standard", "full")

# Culture name -> CultureId in Textresource.xml.
_CULTURES = {"de": "1", "en": "2", "fr": "3", "it": "4", "ru": "5", "nl": "6"}

# Identification reads (0xF8 group, 0xF9 ident, 0xFA hardware, 0xFB software).
# The hub reads these at boot and feeds the device_id text_sensor, so the raw
# datapoints at these addresses are suppressed when --device-id is on.
_IDENT_ADDRESSES = (0x00F8, 0x00F9, 0x00FA, 0x00FB)

# Canonical Vitotronic error-history block address (10 slots x 9 bytes at 0x7507,
# slot index at 0x7561). Used as a fallback signal if the token isn't matched.
_ERROR_HISTORY_ADDRESS = 0x7507

# Built-in openv/community error-code map for VScotHO1-class Vitotronic units.
# The Vitosoft export ships NO code map for the 0x7507 slot, so this is sourced
# from the openv wiki / the project's own example config and is a sensible
# DEFAULT only -- verify/extend for the specific unit. Disable with
# --no-error-codes to emit an empty placeholder instead.
OPENV_ERROR_CODES = {
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


@dataclass
class EventValue:
    name: str = ""
    enum_address_value: int | None = None
    enum_replace_value: str = ""
    unit: str = ""
    lower: str = ""
    upper: str = ""
    stepping: str = ""


@dataclass
class Event:
    id: str
    name: str  # human display (resolved label or technical token)
    address: int | None
    conversion: str
    access_type: int  # 1=ro, 2=rw, 3=wo
    block_length: int | None
    byte_length: int | None
    byte_position: int | None
    bit_length: int | None
    bit_position: int | None
    lower: str = ""
    upper: str = ""
    stepping: str = ""
    enum_type: bool = False
    unit: str = ""  # resolved unit from the access layer (fallback for _unit_for)
    tech: str = ""  # stable Viessmann technical identifier (the join key; -> id:)
    token: str = ""  # raw DPDefinitions Address token (e.g. "ecnsysEventType~Error")
    values: list = field(default_factory=list)


# --- low-level helpers -----------------------------------------------------


def _local(tag: str) -> str:
    """Local tag name, dropping any '{namespace}' prefix ElementTree adds."""
    return tag.rsplit("}", 1)[-1]


def _child(elem, name: str):
    """Text of the first direct child whose LOCAL name matches (namespace-safe)."""
    for c in elem:
        if _local(c.tag) == name:
            return None if c.text is None else c.text.strip()
    return None


def _intval(s):
    if s is None or s == "":
        return None
    try:
        return int(s, 0)
    except (ValueError, TypeError):
        try:
            return int(s)
        except (ValueError, TypeError):
            return None


def _hx(s):
    if not s:
        return None
    try:
        return int(s, 16)
    except (ValueError, TypeError):
        return None


def _hexaddr(s):
    """Parse an access-layer address like '0x0886' (or '0886')."""
    if not s:
        return None
    try:
        return int(s.strip(), 16)
    except (ValueError, TypeError):
        return None


def _address_from_token(token: str):
    """Optolink address from a token like 'BetriebsstundenBrennerGWG~0x0886'."""
    if token and "~" in token:
        tail = token.rsplit("~", 1)[1]
        try:
            return int(tail, 16)
        except ValueError:
            return None
    return None


def _address_from_name(name: str):
    return _address_from_token(name)


def _strip_default_ns(text: str) -> str:
    """Drop the default xmlns so plain local-name lookups work everywhere."""
    return re.sub(r'\sxmlns="[^"]+"', "", text)


def _read_text(path: str) -> str:
    with open(path, "rb") as fh:
        raw = fh.read()
    enc = "utf-16" if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8"
    return raw.decode(enc, errors="replace")


def _find_file(data_dir, *names):
    for n in names:
        p = os.path.join(data_dir, n)
        if os.path.exists(p):
            return p
    return None


def _find_dpdefinitions(data_dir):
    hits = []
    for dirpath, _dirs, files in os.walk(data_dir):
        for f in files:
            if re.match(r"DPDefinitions.*\.xml$", f, re.IGNORECASE):
                hits.append(os.path.join(dirpath, f))
    return sorted(hits)[0] if hits else None


# --- text / unit resolution ------------------------------------------------


def _load_textresources(path, culture="de"):
    """Build {Label: Value} from Textresource.xml, preferring *culture*."""
    if not path:
        return {}
    try:
        root = ET.fromstring(_strip_default_ns(_read_text(path)))
    except ET.ParseError:
        return {}
    want = _CULTURES.get(culture, "1")
    primary, fallback = {}, {}
    for e in root.iter():
        if _local(e.tag) == "TextResource":
            lab = e.attrib.get("Label")
            val = e.attrib.get("Value")
            if lab is None or val is None:
                continue
            fallback.setdefault(lab, val)
            if e.attrib.get("CultureId") == want:
                primary[lab] = val
    out = dict(fallback)
    out.update(primary)
    return out


def _resolve_label(raw: str, textmap: dict) -> str:
    """Resolve a possibly @@-prefixed label. If unresolved (this export ships no
    eventvaluetype strings in Textresource.xml), trim the ``viessmann.<cat>.``
    noise so an enum option reads as ``Name~N`` rather than the full key."""
    if not raw:
        return raw
    if raw.startswith("@@"):
        key = raw[2:]
        if key in textmap:
            return textmap[key]
        return re.sub(r"^viessmann\.[a-z]+\.(name\.)?", "", key) or key
    return raw


def _resolve_unit(raw: str, textmap: dict) -> str:
    if not raw:
        return ""
    if raw in ECN_UNITS:
        return ECN_UNITS[raw]
    key = raw[2:] if raw.startswith("@@") else raw
    if key in textmap:
        return textmap[key]
    if raw.startswith("ecnUnit."):
        return raw[len("ecnUnit.") :]
    return raw


def _readable(label: str) -> str:
    """Recover the Viessmann technical id from a display-name label (mirrors the
    reference PrintEventsForDatapoint.py _readable). The 2026 export references
    names via '@@viessmann.<type>.name.<TECH>' but ships none of those strings in
    Textresource.xml, so recover <TECH> instead of printing the raw '@@...'.
    Already-resolved (non-@@) values pass through unchanged."""
    if not isinstance(label, str) or not label.startswith("@@"):
        return label
    s = label[2:]
    if ".name." in s:  # ...eventtype.name.Outside_Temp -> Outside_Temp
        return s.split(".name.", 1)[1]
    parts = s.split(".")  # ...eventvaluetype.K52_KonfiWeiche~0 -> K52_KonfiWeiche~0
    if len(parts) >= 3 and parts[0] in ("viessmann", "econtrolnet"):
        return ".".join(parts[2:])
    return s


def _tech_id(name_field: str, token: str) -> str:
    """Stable technical identifier (the join key back to Viessmann; drives id:).
    Prefer the Address token stem -- it is the literal key into ecnEventType.xml
    -- and fall back to the @@ display-name key only when no token is present."""
    if token:
        stem = token.split("~", 1)[0].strip()
        if stem:
            return stem
    if name_field and name_field.startswith("@@"):
        return _readable(name_field)
    if name_field and not name_field.startswith(("viessmann.", "econtrolnet.")):
        return name_field.split("~", 1)[0]
    return name_field or ""


# --- access + identification loaders ---------------------------------------


def _load_access(path):
    """token (<ID>) -> access dict from ecnEventType.xml. {} if absent."""
    if not path:
        return {}
    acc = {}
    for e in ET.fromstring(_strip_default_ns(_read_text(path))).iter():
        if _local(e.tag) == "EventType":
            i = _child(e, "ID")
            if not i:
                continue
            acc[i] = dict(
                address=_child(e, "Address"),
                block_length=_child(e, "BlockLength"),
                byte_length=_child(e, "ByteLength"),
                bit_length=_child(e, "BitLength"),
                bit_position=_child(e, "BitPosition"),
                byte_position=_child(e, "BytePosition"),
                conversion=_child(e, "Conversion"),
                conversion_factor=_child(e, "ConversionFactor"),
                unit=_child(e, "Unit"),
            )
    return acc


def _load_identification(path):
    """List of identification rows from ecnDataPointType.xml. [] if absent."""
    if not path:
        return []
    rows = []
    for e in ET.fromstring(_strip_default_ns(_read_text(path))).iter():
        if _local(e.tag) in ("DataPointType", "ecnDataPointType"):
            tok = _child(e, "ID") or _child(e, "Address")
            if not tok:
                continue
            ident = _child(e, "Identification")
            ext = _child(e, "IdentificationExtension")
            extt = _child(e, "IdentificationExtensionTill")
            rows.append(
                dict(
                    token=tok,
                    Identification=ident,
                    IdentificationExtension=ext,
                    IdentificationExtensionTill=extt,
                    ident=_hx(ident),
                    ext=_hx(ext),
                    extt=_hx(extt),
                    f0=_hx(_child(e, "F0")),
                    f0t=_hx(_child(e, "F0Till")),
                )
            )
    return rows


# --- catalog ---------------------------------------------------------------

# Element local-name prefixes that denote a table row or dataset container;
# clearing these during streaming bounds memory on the 212 MB file. Scalar
# fields (Id, Name, DataPointTypeId, ...) never start with these.
_CONTAINER_PREFIXES = ("ecn", "sys", "vsm", "Vsm", "ECN", "Document")

_WANTED_ROWS = {
    "ecnDatapointType",
    "ecnDataPointTypeEventTypeLink",
    "ecnEventType",
    "ecnEventTypeEventValueTypeLink",
    "ecnEventValueType",
}


class Catalog:
    """Parsed Vitosoft export: device types + events (merged with the access
    layer) + value types + identification."""

    def __init__(self, types, dp_name, links, raw_events, vlinks, raw_values,
                 access, ident_rows, textmap):
        self.devices = types  # token -> datapoint-type Id
        self._dp_name = dp_name  # Id -> display name
        self._links = links  # dp_id -> [event_id]
        self._vlinks = vlinks  # event_id -> [value_id]
        self._access = access  # token -> access dict
        self._ident = ident_rows
        self._text = textmap
        self._values = {vid: self._mk_value(f) for vid, f in raw_values.items()}
        self._events = {eid: self._mk_event(eid, f) for eid, f in raw_events.items()}

    # -- assembly --
    def _mk_value(self, f):
        return EventValue(
            name=_resolve_label(f.get("Name", ""), self._text),
            enum_address_value=_intval(f.get("EnumAddressValue")),
            enum_replace_value=_resolve_label(f.get("EnumReplaceValue", ""), self._text),
            unit=_resolve_unit(f.get("Unit", ""), self._text),
            lower=f.get("LowerBorder", "") or "",
            upper=f.get("UpperBorder", "") or "",
            stepping=f.get("Stepping", "") or "",
        )

    def _mk_event(self, eid, f):
        token = f.get("Address", "") or ""
        acc = self._access.get(token, {})

        def pick(acc_key, dp_key):
            v = acc.get(acc_key)
            if v is None or v == "":
                v = f.get(dp_key)
            return _intval(v)

        raw_name = f.get("Name", "") or ""
        addr = (
            _hexaddr(acc.get("address"))
            or _address_from_token(token)
            or _address_from_name(raw_name)
        )
        return Event(
            id=eid,
            name=_friendly(raw_name, self._text),
            address=addr,
            conversion=(f.get("Conversion") or acc.get("conversion") or ""),
            access_type=_intval(f.get("Type")) or 1,
            block_length=pick("block_length", "BlockLength"),
            byte_length=pick("byte_length", "ByteLength"),
            byte_position=pick("byte_position", "BytePosition"),
            bit_length=pick("bit_length", "BitLength"),
            bit_position=pick("bit_position", "BitPosition"),
            lower=f.get("LowerBorder", "") or "",
            upper=f.get("UpperBorder", "") or "",
            stepping=f.get("Stepping", "") or "",
            enum_type=(f.get("EnumType", "") or "").lower() in ("1", "true"),
            unit=_resolve_unit(acc.get("unit", ""), self._text),
            tech=_tech_id(raw_name, token),
            token=token,
        )

    # -- queries --
    def events_for(self, device_token: str) -> list:
        dp_id = self.devices.get(device_token)
        if dp_id is None:
            return []
        out = []
        for ev_id in self._links.get(dp_id, []):
            ev = self._events.get(ev_id)
            if ev is None:
                continue
            ev.values = []  # rebuild (idempotent across repeated calls)
            for val_id in self._vlinks.get(ev_id, []):
                val = self._values.get(val_id)
                if val is not None:
                    ev.values.append(val)
            out.append(ev)
        return out

    def identification_for(self, token: str) -> dict:
        for r in self._ident:
            if r.get("token") == token:
                return r
        return {}

    def resolve(self, device_id: int, hw: int, sw: int, f0: int | None = None):
        """Map live identification bytes to a datapoint-type token.

        Algorithm (validated against the real export):
          1. Filter by ``Identification == device_id`` -> the device family.
          2. The high byte of IdentificationExtension is a *categorical* hardware-
             generation tag (only one device id in the whole dataset ever uses
             more than one high byte). So:
               * if the live HW byte appears among the family's entries, keep only
                 those entries (equality);
               * otherwise (e.g. HW=0x03 while the family is all 0x01) the byte is
                 the family's constant nominal value -> drop it and match on the
                 software index alone. Folding HW into a (HW<<8|SW) magnitude
                 comparison is wrong: it inflates the value past the SW ranges and
                 selects a far-later revision.
          3. Match the SOFTWARE index (low byte) against each entry's
             ext..extt range; "from software index N onwards" semantics. If SW is
             beyond all known ranges, pick the latest (floor); if below all, the
             lowest.
          4. If several entries share the SW range, disambiguate by F0
             (ProtocolIdentifierOffset); without F0, prefer the generic variant.

        Returns (token, reason) or (None, reason).
        """
        fam = [t for t in self._ident if t["ident"] == device_id]
        if not fam:
            return None, f"no datapoint type with Identification == 0x{device_id:04X}"
        have_ext = [t for t in fam if t["ext"] is not None]
        if not have_ext:
            return fam[0]["token"], "Identification match (no IdentificationExtension in catalog)"

        highs = {t["ext"] >> 8 for t in have_ext}
        note = ""
        if hw in highs:
            cand = [t for t in have_ext if (t["ext"] >> 8) == hw]
        else:
            cand = have_ext
            note = f" (HW 0x{hw:02X} not in catalog; matched on software index only)"

        # ignore the rare multi-high-byte spans; keep single-high-byte ranges
        single = [t for t in cand if t["extt"] is None or (t["ext"] >> 8) == (t["extt"] >> 8)]
        cand = single or cand

        def sw_range(t):
            lo = t["ext"] & 0xFF
            hi = (t["extt"] & 0xFF) if t["extt"] is not None else lo
            return lo, hi

        contain = [t for t in cand if sw_range(t)[0] <= sw <= sw_range(t)[1]]
        if contain:
            pool, why = contain, "software-index range"
        else:
            below = sorted([t for t in cand if sw_range(t)[0] <= sw], key=lambda t: sw_range(t)[0])
            if below:
                pool = [below[-1]]
                why = "software-index floor (SW beyond catalog ranges; latest revision)"
            else:
                pool = [sorted(cand, key=lambda t: sw_range(t)[0])[0]]
                why = "lowest software index (SW below catalog ranges)"

        f0_aware = [t for t in pool if t["f0"] is not None]
        if f0_aware:
            if f0 is not None:
                fm = [
                    t
                    for t in f0_aware
                    if t["f0"] <= f0 <= (t["f0t"] if t["f0t"] is not None else t["f0"])
                ]
                if fm:
                    return fm[0]["token"], why + " + F0 match" + note
            generic = sorted(pool, key=lambda t: (t["f0"] if t["f0"] is not None else -1))
            tail = " (F0 not provided; generic variant)" if f0 is None else " (no F0 match; generic variant)"
            return generic[0]["token"], why + tail + note
        return pool[0]["token"], why + note


def load_catalog(data_dir: str, culture: str = "de") -> Catalog:
    """Load the Vitosoft export under *data_dir* (streams DPDefinitions)."""
    dp_path = _find_dpdefinitions(data_dir)
    if not dp_path:
        raise SystemExit(
            f"no DPDefinitions*.xml found under {data_dir!r}. "
            "Point --data at the Vitosoft export directory (unzip DPDefinitions.zip first)."
        )
    access = _load_access(_find_file(data_dir, "ecnEventType.xml"))
    ident = _load_identification(_find_file(data_dir, "ecnDataPointType.xml"))
    textmap = _load_textresources(
        _find_file(data_dir, f"Textresource_{culture}.xml", "Textresource.xml"), culture
    )
    if not access:
        print(
            "warning: ecnEventType.xml not found next to DPDefinitions.xml; "
            "lengths/bit masks/addresses may be incomplete (the 2026 export keeps "
            "them there, not in DPDefinitions.xml).",
            file=sys.stderr,
        )

    types, dp_name = {}, {}
    links, vlinks = defaultdict(list), defaultdict(list)
    raw_events, raw_values = {}, {}

    for _event, el in ET.iterparse(dp_path, events=("end",)):
        ln = _local(el.tag)
        if ln in _WANTED_ROWS:
            f = {_local(c.tag): (c.text or "").strip() for c in el}
            if ln == "ecnDatapointType":
                dp_id = f.get("Id")
                if dp_id:
                    token = f.get("Address") or f.get("Name") or dp_id
                    types[token] = dp_id
                    dp_name[dp_id] = f.get("Name") or token
            elif ln == "ecnDataPointTypeEventTypeLink":
                d, e = f.get("DataPointTypeId"), f.get("EventTypeId")
                if d and e:
                    links[d].append(e)
            elif ln == "ecnEventType":
                if f.get("Id"):
                    raw_events[f["Id"]] = f
            elif ln == "ecnEventTypeEventValueTypeLink":
                e = f.get("EventTypeId")
                v = f.get("EventValueId") or f.get("EventValueTypeId")  # real field is EventValueId
                if e and v:
                    vlinks[e].append(v)
            elif ln == "ecnEventValueType":
                if f.get("Id"):
                    raw_values[f["Id"]] = f
        if ln.startswith(_CONTAINER_PREFIXES):
            el.clear()  # free table rows / datasets we have finished reading

    return Catalog(types, dp_name, links, raw_events, vlinks, raw_values, access, ident, textmap)


# --- entity emission -------------------------------------------------------


def _friendly(label: str, textmap: dict | None = None) -> str:
    """Friendly entity name (mirrors the reference PrintEventsForDatapoint.py
    _friendly): a resolved translation verbatim, else the technical id with
    '_' -> spaces. Deliberately light -- camelCase / coding-prefix compounds
    (A1M1, K00_, WW, RT) are kept, so a real translation source stays preferable
    where clean labels matter. The stable technical id is carried separately as
    the entity ``id:``."""
    if textmap and isinstance(label, str) and label.startswith("@@"):
        hit = textmap.get(label[2:])
        if hit:
            return hit
    if isinstance(label, str) and label and not label.startswith("@@"):
        # a real translation passes through; a raw "Tech~0xADDR" token (older
        # export / fixture style) is humanised like the technical-id fallback.
        if "~0x" in label:
            return label.split("~", 1)[0].replace("_", " ").strip()
        return label
    return _readable(label).replace("_", " ").strip()


def _make_obj_id(tech: str, used: set) -> str:
    """A unique, valid ESPHome id from a technical identifier. Preserves the
    technical name (it is the join key back to Viessmann); sanitises only what
    ESPHome forbids and de-duplicates with a numeric suffix."""
    base = re.sub(r"[^0-9A-Za-z_]", "_", tech or "")
    base = re.sub(r"_+", "_", base).strip("_")
    if not base or base[0].isdigit():
        base = ("id_" + base) if base else "id_dp"
    cand = base
    i = 2
    while cand in used:
        cand = f"{base}_{i}"
        i += 1
    used.add(cand)
    return cand


def _poll_for(ev: Event, conv_kind: str) -> int:
    if ev.access_type in (2, 3):
        return POLL_CODING
    if conv_kind == COUNTER:
        return POLL_SLOW
    return POLL_FAST


def _enum_options(ev: Event) -> list:
    """Return [(raw_value, label)] from the event's value types, if any."""
    out = []
    for v in ev.values:
        if v.enum_address_value is not None:
            label = v.enum_replace_value or v.name or f"0x{v.enum_address_value:02X}"
            out.append((v.enum_address_value, label))
    return out


def _unit_for(ev: Event) -> str:
    for v in ev.values:
        if v.unit:
            return v.unit
    return ev.unit or ""


def _yaml_str(s: str) -> str:
    # Always double-quote and escape, so umlauts / specials never break parsing.
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def emit_entity(ev: Event, profile: str):
    """Return (platform, yaml_lines) for one event, or None to skip it.
    ev.name is the resolved friendly name; the stable technical ``id:`` is added
    by generate() (which can de-duplicate across the whole package)."""
    name = ev.name
    addr = ev.address
    if addr is None:
        return None

    # Non-numeric conversions: surface as a commented hint, never a wrong sensor.
    if ev.conversion in NON_NUMERIC_CONVERSIONS:
        hint = NON_NUMERIC_CONVERSIONS[ev.conversion]
        return ("comment", [f"# {name} @ 0x{addr:04X}: {ev.conversion} ({hint})"])

    conv, conv_kind = CONVERSION_MAP.get(ev.conversion, (None, PLAIN))
    note = None
    if conv is None:
        conv = "noconv"
        note = f"conversion {ev.conversion!r} has no preset; raw noconv emitted"

    length = ev.block_length or ev.byte_length or 1
    enum_opts = _enum_options(ev)
    is_bit = (ev.bit_length or 0) > 0
    writable = ev.access_type in (2, 3)

    # A wide plain numeric is almost certainly an array/struct/schedule; the
    # numeric converters only handle 1..4 bytes -> comment rather than emit garbage.
    if length > 4 and not is_bit and not enum_opts:
        return (
            "comment",
            [f"# {name} @ 0x{addr:04X}: {ev.conversion or 'raw'} length {length} > 4 bytes -> custom decode"],
        )
    # Scalar reads are clamped to the converter-supported 1..4 bytes; the bit
    # path (below) keeps the real block length and is capped separately.
    num_length = length if length in (1, 2, 3, 4) else max(1, min(4, length))

    poll = _poll_for(ev, conv_kind)
    lines: list[str] = []
    if note:
        lines.append(f"  # NOTE: {note}")

    # --- writable: select (enum) or number ---
    if writable and enum_opts and length == 1 and all(0 <= v <= 255 for v, _ in enum_opts):
        lines += [
            "- platform: vitohome",
            f"  name: {_yaml_str(name)}",
            f"  address: 0x{addr:04X}",
            f"  length: {length}",
            "  disabled_by_default: true",
            f"  update_interval: {poll}s",
            "  options:",
        ]
        for v, label in enum_opts:
            lines.append(f"    0x{v:02X}: {_yaml_str(label)}")
        return ("select", lines)

    if writable:
        lo = ev.lower or (ev.values[0].lower if ev.values else "")
        hi = ev.upper or (ev.values[0].upper if ev.values else "")
        step = ev.stepping or (ev.values[0].stepping if ev.values else "")
        signed = conv in ("div2", "div10")  # Vitosoft signed conversions
        lines += [
            "- platform: vitohome",
            f"  name: {_yaml_str(name)}",
            f"  address: 0x{addr:04X}",
            f"  length: {length}",
            f"  converter: {conv}",
        ]
        if signed and conv == "noconv":
            lines.append("  signed: true")
        unit = _unit_for(ev)
        if unit:
            lines.append(f"  unit_of_measurement: {_yaml_str(unit)}")
        if lo != "" and hi != "" and step not in ("", "0"):
            lines.append(f"  min_value: {lo}")
            lines.append(f"  max_value: {hi}")
            lines.append(f"  step: {step}")
        else:
            lines.append("  # Borders absent in this export row. These placeholders")
            lines.append("  # pass `esphome config` but pin the value to 0 - set real")
            lines.append("  # min_value/max_value/step from the datapoint before use.")
            lines.append("  min_value: 0")
            lines.append("  max_value: 0")
            lines.append("  step: 1")
        lines.append("  mode: box")
        lines.append("  disabled_by_default: true")
        lines.append(f"  update_interval: {poll}s")
        return ("number", lines)

    # --- read-only ---
    if is_bit:
        # BitPosition is the ABSOLUTE bit index within the block; BytePosition is
        # sometimes left 0 in the export, so derive the byte from the bit index.
        bit_pos = ev.bit_position or 0
        byte_off = bit_pos // 8
        mask = 1 << (bit_pos % 8)
        # The component reads <=4 bytes with byte_offset 0..3 (and offset <
        # length). A bit deeper in a large status block can't be expressed ->
        # surface it as a comment instead of emitting config that fails validation.
        if length > 4 or byte_off > 3 or byte_off >= length:
            return (
                "comment",
                [
                    f"# {name} @ 0x{addr:04X}: bit {bit_pos} of a {length}-byte block "
                    f"(byte {byte_off}) exceeds binary_sensor length/offset limits -> custom handling"
                ],
            )
        lines += [
            "- platform: vitohome",
            f"  name: {_yaml_str(name)}",
            f"  address: 0x{addr:04X}",
            f"  length: {length}",
        ]
        if byte_off:
            lines.append(f"  byte_offset: {byte_off}")
        lines.append(f"  bit_mask: 0x{mask:02X}")
        lines.append("  disabled_by_default: true")
        lines.append(f"  update_interval: {poll}s")
        return ("binary_sensor", lines)

    if enum_opts:
        lines += [
            "- platform: vitohome",
            "  type: enum",
            f"  name: {_yaml_str(name)}",
            f"  address: 0x{addr:04X}",
            f"  length: {num_length}",
            "  disabled_by_default: true",
            f"  update_interval: {poll}s",
            "  options:",
        ]
        for v, label in enum_opts:
            lines.append(f"    0x{v:02X}: {_yaml_str(label)}")
        return ("text_sensor", lines)

    # numeric sensor
    signed = conv in ("div2", "div10")
    lines += [
        "- platform: vitohome",
        f"  name: {_yaml_str(name)}",
        f"  address: 0x{addr:04X}",
        f"  length: {length}",
        f"  converter: {conv}",
    ]
    if signed and conv == "noconv":
        lines.append("  signed: true")
    unit = _unit_for(ev)
    if unit:
        lines.append(f"  unit_of_measurement: {_yaml_str(unit)}")
    if conv_kind == COUNTER:
        lines.append("  state_class: total_increasing")
        lines.append("  accuracy_decimals: 1")
    else:
        lines.append("  state_class: measurement")
    lines.append("  disabled_by_default: true")
    lines.append(f"  update_interval: {poll}s")
    return ("sensor", lines)


def _is_identification(ev: Event) -> bool:
    return ev.address in _IDENT_ADDRESSES


def _is_error_history(ev: Event) -> bool:
    """The most-recent error slot (ecnsysEventType~Error), not the index slot."""
    if ev.address is None:
        return False
    tok = ev.token or ""
    if "ecnsysEventType" in tok and "Error" in tok and "Index" not in tok:
        return True
    return ev.address == _ERROR_HISTORY_ADDRESS and "Index" not in tok


def _is_mode_select(ev: Event) -> bool:
    return ev.access_type in (2, 3) and bool(
        re.search(r"Betriebsart|BedienteilBA|BedienBetriebsart", ev.token or ev.tech or "", re.I)
    )


def _device_id_lines(oid: str) -> list[str]:
    """The hub-fed device identity text_sensor (no address)."""
    return [
        "- platform: vitohome",
        "  type: device_id",
        '  name: "Geraete-Typ"',
        f"  id: {oid}",
        '  icon: "mdi:identifier"',
        "  entity_category: diagnostic",
    ]


def _error_history_lines(ev: Event, oid: str, codes: bool) -> list[str]:
    """The most-recent error slot as a 9-byte block with full timestamp decode.
    The Vitosoft block is 90 bytes (10 slots x 9); this reads slot 0 only,
    matching the component's error_history type and the project example."""
    lines = [
        "# Error history. Full block is 90 bytes (10 slots x 9) at this address,",
        "# plus a slot index at 0x7561; this reads the most-recent slot only.",
        "# If the device NAKs the 9-byte block read, drop length to 1 (code only).",
        "- platform: vitohome",
        "  type: error_history",
        '  name: "Letzter Fehler"',
        f"  id: {oid}",
        f"  address: 0x{ev.address:04X}",
        "  length: 9",
        '  icon: "mdi:alert-circle"',
        f"  update_interval: {POLL_ERROR}s",
    ]
    if codes:
        lines.append("  # openv/community code map (DEFAULT - verify for this unit):")
        lines.append("  codes:")
        for code, text in sorted(OPENV_ERROR_CODES.items()):
            lines.append(f"    0x{code:02X}: {_yaml_str(text)}")
    else:
        lines.append("  # codes: { 0x00: \"kein Fehler\", ... }  # add a code->text map")
    return lines


def _mode_select_caveat() -> list[str]:
    return [
        "# Writable operating-mode (Betriebsart). KNOWN ISSUES on real 0x20CB /",
        "# VScotHO1 units (see project notes / hardware audit):",
        "#  * openv/community drives the mode at 0x2301 with 6 options",
        "#    (Warmwasser/Reduziert/Normal/Heizen+WW/Heizen+WW FS/Abschaltbetrieb);",
        "#    Vitosoft lists this variant's write here with fewer options.",
        "#  * Writes may be ACKed yet not take effect: a Bedienteil program-switch",
        "#    interlock, a read/write address split, or a firmware-accepted value",
        "#    subset narrower than the enum. Verify with `debug:` under uart:.",
        "#  * The 'aktuelle Betriebsart' READ (a bit-field in the 0x2500 block)",
        "#    does NOT decode correctly and is intentionally not emitted.",
    ]


def _profile_keep(ev: Event, profile: str) -> bool:
    if profile == "full":
        return True
    conv, conv_kind = CONVERSION_MAP.get(ev.conversion, (None, PLAIN))
    writable = ev.access_type in (2, 3)
    if profile == "standard":
        return ev.conversion not in NON_NUMERIC_CONVERSIONS
    if profile == "minimal":
        if writable:
            return True
        if conv_kind in (DIV, COUNTER):
            return True
        return False
    return True


def generate(catalog: Catalog, device: str, profile: str,
             include_re: str | None, exclude_re: str | None) -> str:
    events = catalog.events_for(device)
    if not events:
        raise SystemExit(
            f"device {device!r} not found or has no events. "
            "Run with --list-devices to see available device tokens."
        )

def generate(catalog: Catalog, device: str, profile: str,
             include_re: str | None, exclude_re: str | None,
             emit_device_id: bool = True, emit_error_history: bool = True,
             error_codes: bool = True) -> str:
    events = catalog.events_for(device)
    if not events:
        raise SystemExit(
            f"device {device!r} not found or has no events. "
            "Run with --list-devices to see available device tokens."
        )

    inc = re.compile(include_re) if include_re else None
    exc = re.compile(exclude_re) if exclude_re else None

    buckets: dict[str, list[str]] = {
        "sensor": [],
        "binary_sensor": [],
        "number": [],
        "select": [],
        "text_sensor": [],
    }
    comments: list[str] = []
    seen_addr: set[int] = set()
    used_ids: set[str] = set()
    kept = 0

    # Hub-fed device identity (covers the 0xF8..0xFB identification reads, which
    # are then suppressed below).
    if emit_device_id:
        oid = _make_obj_id("device_type", used_ids)
        buckets["text_sensor"].extend("  " + ln if ln else "" for ln in _device_id_lines(oid))
        kept += 1

    for ev in sorted(events, key=lambda e: (e.address or 0)):
        # Special datapoints are handled independently of profile/name filters.
        if emit_device_id and _is_identification(ev):
            continue  # represented by the device_id entity above
        if emit_error_history and _is_error_history(ev):
            if ev.address is not None and ev.address in seen_addr:
                continue
            if ev.address is not None:
                seen_addr.add(ev.address)
            oid = _make_obj_id("letzter_fehler", used_ids)
            buckets["text_sensor"].extend(
                "  " + ln if ln else "" for ln in _error_history_lines(ev, oid, error_codes)
            )
            kept += 1
            continue

        if not _profile_keep(ev, profile):
            continue
        target = ev.name or ""
        if inc and not inc.search(target):
            continue
        if exc and exc.search(target):
            continue
        if ev.address is not None and ev.address in seen_addr:
            continue

        result = emit_entity(ev, profile)
        if result is None:
            continue
        platform, lines = result
        if platform == "comment":
            comments.extend(lines)
            continue
        if ev.address is not None:
            seen_addr.add(ev.address)
        # Writable operating-mode select: prepend the hardware caveat.
        if platform == "select" and _is_mode_select(ev):
            lines = _mode_select_caveat() + lines
        # Carry the stable technical identifier as id: (the join key back to
        # Viessmann), inserted right after the entity's name.
        oid = _make_obj_id(ev.tech or ev.name, used_ids)
        for idx, ln in enumerate(lines):
            if ln.startswith("  name:"):
                lines.insert(idx + 1, f"  id: {oid}")
                break
        buckets[platform].extend("  " + ln if ln else "" for ln in lines)
        kept += 1

    out: list[str] = []
    out.append("# ============================================================")
    out.append(f"# vitohome package generated for device: {device}")
    out.append(f"# profile: {profile}   entities: {kept}")
    out.append("#")
    out.append("# Generated by scripts/gen_catalog.py from a Vitosoft XML export.")
    out.append("# Define the `vitohome:` hub and `uart:` in your device YAML and")
    out.append("# include this file via `packages:`. All entities are")
    out.append("# disabled_by_default; enable the ones you want in Home Assistant.")
    out.append("#")
    out.append("# VERIFY ON HARDWARE: not every address answers on every firmware.")
    out.append("# Run `esphome config` then `esphome compile`/`run` before relying")
    out.append("# on any value. number entities whose borders were absent in the")
    out.append("# export get min/max/step = 0/0/1 placeholders: valid config, but")
    out.append("# pinned to 0 until you fill in the real bounds.")
    out.append("# ============================================================")
    out.append("")

    for platform in ("sensor", "binary_sensor", "number", "select", "text_sensor"):
        if buckets[platform]:
            out.append(f"{platform}:")
            out.extend(buckets[platform])
            out.append("")

    if comments:
        out.append("# --- datapoints needing custom decode (not auto-emitted) ---")
        out.extend(comments)
        out.append("")

    return "\n".join(out)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--data", required=True, help="dir with the Vitosoft XML export (DPDefinitions*.xml, ecnEventType.xml, ...)")
    p.add_argument("--device", help="datapoint-type token to generate for, e.g. VScotHO1_72")
    p.add_argument("--identify", help="device Identification hex (group<<8|ident), e.g. 0x20CB; auto-selects the revision")
    p.add_argument("--hw", help="hardware index hex (0xFA), used with --identify")
    p.add_argument("--sw", help="software index hex (0xFB), used with --identify")
    p.add_argument("--f0", help="protocol identifier offset hex (0xF0), optional, used with --identify")
    p.add_argument("--list-devices", action="store_true", help="list device tokens (+ identification ranges) and exit")
    p.add_argument("--profile", choices=PROFILES, default="standard", help="how many datapoints to emit (default: standard)")
    p.add_argument("--include", help="regex; only emit events whose name matches")
    p.add_argument("--exclude", help="regex; drop events whose name matches")
    p.add_argument("--culture", default="de", help="Textresource language for names/labels (de,en,fr,it,ru,nl)")
    p.add_argument("--device-id", action=argparse.BooleanOptionalAction, default=True,
                   help="emit a device_id diagnostic text_sensor and suppress the raw 0xF8..0xFB reads (default: on)")
    p.add_argument("--error-history", action=argparse.BooleanOptionalAction, default=True,
                   help="emit a 'Letzter Fehler' error_history text_sensor for the 0x7507 slot (default: on)")
    p.add_argument("--error-codes", action=argparse.BooleanOptionalAction, default=True,
                   help="include the built-in openv error-code map in the error_history entity (default: on)")
    p.add_argument("--out", help="output file (default: stdout)")
    args = p.parse_args(argv)

    catalog = load_catalog(args.data, culture=args.culture)

    if args.list_devices:
        for token in sorted(catalog.devices):
            dp_id = catalog.devices[token]
            display = _friendly(catalog._dp_name.get(dp_id, ""), catalog._text)
            n = len(catalog.events_for(token))
            row = catalog.identification_for(token)
            tail = ""
            if row.get("Identification"):
                tail = f"\tident={row['Identification']}"
                if row.get("IdentificationExtension"):
                    rng = row["IdentificationExtension"]
                    if row.get("IdentificationExtensionTill"):
                        rng += f"-{row['IdentificationExtensionTill']}"
                    tail += f" ext={rng}"
            print(f"{token}\t{display}\t({n} events){tail}")
        return 0

    device = args.device
    if not device and args.identify:
        if not (args.hw and args.sw):
            p.error("--identify requires --hw and --sw")
        token, why = catalog.resolve(
            int(args.identify, 16),
            int(args.hw, 16),
            int(args.sw, 16),
            int(args.f0, 16) if args.f0 else None,
        )
        if token is None:
            print(f"could not resolve device: {why}", file=sys.stderr)
            return 1
        print(f"resolved {args.identify}/HW{args.hw}/SW{args.sw} -> {token}  [{why}]", file=sys.stderr)
        device = token

    if not device:
        p.error("provide --device, or --identify with --hw/--sw (or use --list-devices)")

    text = generate(
        catalog, device, args.profile, args.include, args.exclude,
        emit_device_id=args.device_id,
        emit_error_history=args.error_history,
        error_codes=args.error_codes,
    )
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(text)
        print(f"wrote {args.out}", file=sys.stderr)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
