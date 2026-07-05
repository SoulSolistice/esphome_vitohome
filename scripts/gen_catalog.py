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
import math
import os
import re
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict
from dataclasses import dataclass, field

# fault_codes.py is a sibling module (this file is run as a script, so its own
# directory may not be on sys.path when invoked from elsewhere). Add it, then
# import the fault-code maps from the single source of truth.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fault_codes  # noqa: E402  (sibling import after path insert)

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
    "RotateBytes": ("rotatebytes", PLAIN),  # big-endian 2-byte values -> decode_scaled_be
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
    "MultOffsetFloat": "float -> custom decode",
}

# Mode commands whose live state is read at a DIFFERENT address than the
# command register (see the read/write-split analysis). Maps the writable
# command address -> the read-only effective-state address, per heating
# circuit. Emitted as `state_address` on the select so its read-back reflects
# the actual state, not the command register. Betriebsart (0x2323) is NOT here:
# its read (0x2501, 4 values) and write (0x2323, 5 values) domains differ, so
# it stays a command-only select plus a separate effective-mode sensor.
COMMAND_STATE_ADDR = {
    0x2330: 0x2303,  # A1: party  (NRx Partybetrieb  -> BedienPartybetrieb)
    0x2331: 0x2302,  # A1: economy (NRx Sparbetrieb  -> BedienSparbetrieb)
    0x3330: 0x3303,  # M2: party
    0x3331: 0x3302,  # M2: economy
    0x4330: 0x4303,  # M3: party
    0x4331: 0x4302,  # M3: economy
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
# slot index at 0x7561). Used as a fallback only; per-slot FehlerHis* tokens take
# precedence (see _is_error_history) because some units (e.g. VScotHO1_72) carry
# their fault log at FehlerHisFA01..20 (0x7590..0x763B) and have NO 0x7507 event.
_ERROR_HISTORY_ADDRESS = 0x7507

# Fault-code maps (OPENV / VITOTRONIC_VD200 / UNION) and the openv-vs-VD200
# CONFLICTS live in fault_codes.py -- a single source of truth, imported above as
# `fault_codes`. The --error-code-set flag selects which map is attached to
# error_history entities (see _error_history_lines / generate).


@dataclass
class EventValue:
    name: str = ""
    enum_address_value: int | None = None
    enum_replace_value: str = ""
    # ecnEventValueType.Description: the ALREADY-RESOLVED human-readable label
    # (e.g. "OK", "Vitodens mit Vitotronic 100 HC1"). Unlike enum_replace_value /
    # name, this needs no Textresource lookup -- and this export ships no
    # eventvaluetype strings in Textresource, so it is the only reliable label
    # source for enum options. Preferred first by _enum_options().
    description: str = ""
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
    fc_read: str = ""  # access function code for reads (Virtual_READ, GFA_READ, ...)
    fc_write: str = ""  # access function code for writes (Virtual_WRITE, ...)
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
                fc_read=_child(e, "FCRead"),
                fc_write=_child(e, "FCWrite"),
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

    def __init__(self, types, dp_name, links, raw_events, vlinks, raw_values, access, ident_rows, textmap):
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
            description=(f.get("Description", "") or "").strip(),
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
        addr = _hexaddr(acc.get("address")) or _address_from_token(token) or _address_from_name(raw_name)
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
            fc_read=(acc.get("fc_read") or f.get("FCRead") or ""),
            fc_write=(acc.get("fc_write") or f.get("FCWrite") or ""),
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
                fm = [t for t in f0_aware if t["f0"] <= f0 <= (t["f0t"] if t["f0t"] is not None else t["f0"])]
                if fm:
                    return fm[0]["token"], why + " + F0 match" + note
            generic = sorted(pool, key=lambda t: t["f0"] if t["f0"] is not None else -1)
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
    textmap = _load_textresources(_find_file(data_dir, f"Textresource_{culture}.xml", "Textresource.xml"), culture)
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
    if _is_writable(ev):
        return POLL_CODING
    if conv_kind == COUNTER:
        return POLL_SLOW
    return POLL_FAST


def _enum_options(ev: Event) -> list:
    """Return [(raw_value, label)] from the event's value types, if any.

    Label priority: Description (pre-resolved German, column O of the Vitosoft
    export) -> resolved EnumReplaceValue -> Name -> hex. Description is first
    because this export ships no eventvaluetype strings in Textresource, so the
    token paths resolve only to a trimmed stem (e.g. "Allgemein_Sensorstatus~0")
    while Description already holds "OK". ~87% of this device's enum option
    values carry a Description; the rest fall through to the old behaviour.
    """
    out = []
    for v in ev.values:
        if v.enum_address_value is not None:
            label = v.description or v.enum_replace_value or v.name or f"0x{v.enum_address_value:02X}"
            out.append((v.enum_address_value, label))
    return out


_BOOLEAN_ON_OFF_PAIRS = [
    ("ein", "aus"),
    ("an", "aus"),
    ("aktiv", "inaktiv"),
    ("ja", "nein"),
    ("on", "off"),
    ("yes", "no"),
    ("freigegeben", "gesperrt"),
]


def _norm_bool_label(label: str) -> str:
    """Normalize an enum option label for on/off matching: drop a leading
    coding-value echo ("0 inaktiv", "1: Ja"), lowercase, trim."""
    return re.sub(r"^\d+\s*[:.]?\s*", "", (label or "").strip()).lower()


def _boolean_pair(opts: list):
    """(on_value, off_value, on_label, off_label) when a 2-option enum is a
    semantic on/off toggle, else None.

    Two options alone do NOT make a boolean: K01 {Einkessel, Mehrkessel} or
    K88 {Celsius, Fahrenheit} are choices, and a switch card showing "on"
    would be meaningless for them. A pair qualifies only via:
      * a known on/off label pair after normalization (EIN/AUS, aktiv/inaktiv,
        Ja/Nein, ...), matched in either value order -- so K8A's
        {175: aktiv, 176: inaktiv} works despite its non-0/1 values;
      * a negation pair, one label being "nicht " + the other
        (vorhanden / nicht vorhanden, senden / nicht senden);
      * an unresolved Allgemein_Zustand_Ein_Aus~N token stem (the Neptun pump
        overrides), where ~1 is on and ~0 is off by the type's own name.
    Everything else stays a select.
    """
    if len(opts) != 2:
        return None
    (v_a, l_a), (v_b, l_b) = opts
    raw_a, raw_b = (l_a or ""), (l_b or "")
    # Token-stem rule: the enum type name itself says on/off.
    if "_ein_aus~" in raw_a.lower() and "_ein_aus~" in raw_b.lower():
        pick = {raw_a.rsplit("~", 1)[-1]: (v_a, raw_a), raw_b.rsplit("~", 1)[-1]: (v_b, raw_b)}
        if set(pick) == {"0", "1"}:
            on_v, on_l = pick["1"]
            off_v, off_l = pick["0"]
            return (on_v, off_v, "Ein", "Aus")
    a, b = _norm_bool_label(raw_a), _norm_bool_label(raw_b)
    # Negation rule.
    if a == "nicht " + b:
        return (v_b, v_a, raw_b, raw_a)
    if b == "nicht " + a:
        return (v_a, v_b, raw_a, raw_b)
    # Allowlisted pairs, either order.
    for on_word, off_word in _BOOLEAN_ON_OFF_PAIRS:
        if a == on_word and b == off_word:
            return (v_a, v_b, raw_a, raw_b)
        if a == off_word and b == on_word:
            return (v_b, v_a, raw_b, raw_a)
    return None


def _enum_read_length(enum_opts: list, fallback: int) -> int:
    """Minimal byte width that holds every enum value. A status enum living in a
    larger block (e.g. SM1_Sensor_*_Status_GWG, a 1-byte field inside the 20-byte
    0xCF90 block) must read 1 byte at its BytePosition, not the clamped block
    width -- otherwise the 4-byte read never matches 0/1/2/128. Derive from the
    values; fall back to the clamped length only if there are none."""
    if not enum_opts:
        return fallback
    hi = max(v for v, _ in enum_opts)
    return 1 if hi < 0x100 else (2 if hi < 0x10000 else fallback)


# Unit -> Home Assistant device_class, for proper cards and long-term
# statistics. Grounded in the units the full VScotHO1_72 export actually
# emits; deliberately conservative -- unmapped units (%, K deltas,
# "Prozent pro K", "l pro h", ...) get no device_class rather than a wrong
# one. "mBar" is normalised to HA's canonical "mbar" spelling by _unit_for.
_DEVICE_CLASS_BY_UNIT = {
    "\u00b0C": "temperature",
    "kWh": "energy",
    "Wh": "energy",
    "mbar": "pressure",
    "dBm": "signal_strength",
    "h": "duration",
    "min": "duration",
    "s": "duration",
}


def _device_class_for(unit: str):
    return _DEVICE_CLASS_BY_UNIT.get(unit or "")


def _unit_for(ev: Event) -> str:
    unit = ""
    for v in ev.values:
        if v.unit:
            unit = v.unit
            break
    unit = unit or ev.unit or ""
    # HA's canonical pressure spelling is "mbar"; the export writes "mBar".
    return "mbar" if unit == "mBar" else unit


def _yaml_str(s: str) -> str:
    # Always double-quote and escape, so umlauts / specials never break parsing.
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


# Mirrors CONVERTERS (scale, default_signed) in components/vitohome/__init__.py.
# Kept in sync by hand; the component's number.py is the runtime authority.
_CONV_SCALE = {
    "noconv": 1.0,
    "div2": 0.5,
    "div10": 0.1,
    "div100": 0.01,
    "div1000": 0.001,
    "sec2hour": 1.0 / 3600.0,
    "mult2": 2.0,
    "mult5": 5.0,
    "mult10": 10.0,
    "mult100": 100.0,
}


def _llround(x: float) -> int:
    """Round half away from zero -- the semantics of C++ ``std::llround`` and of
    the component's ``llround`` helper. Python's built-in ``round()`` is half to
    even and diverges at negative half-steps (round(-128.5) == -128 fits int8;
    llround(-128.5) == -129 is rejected)."""
    return int(math.floor(abs(x) + 0.5)) * (1 if x >= 0 else -1)


def _bound_fits(value_str: str, length: int, conv: str, is_signed: bool) -> bool:
    """Whether a min/max border survives encoding into ``length`` raw bytes.

    Mirrors components/vitohome/number.py exactly (raw = llround(value / scale)
    -- half away from zero, matching ``decode.h::encode_scaled`` -- then a
    signed/unsigned range check), so a border the generator emits will not be
    rejected later at ``esphome config`` time. Borders that do not fit (e.g. a
    year-valued bound on a 1-byte field, or a display unit that is not the raw
    encoding) fall back to the safe 0/0/1 placeholder instead.
    """
    try:
        v = float(value_str)
    except (TypeError, ValueError):
        return False
    scale = _CONV_SCALE.get(conv, 1.0)
    raw = _llround(v / scale)
    if is_signed:
        return -(1 << (8 * length - 1)) <= raw <= (1 << (8 * length - 1)) - 1
    return 0 <= raw <= (1 << (8 * length)) - 1


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

    # A datapoint is a field carved from a firmware block: BlockLength is the
    # whole block at `addr`; ByteLength is the field's own width; BytePosition is
    # its offset within the block. The bit path (below) keeps the full block
    # length -- it derives the byte offset from the absolute bit index. Every
    # scalar path reads `length` bytes from an address with no offset handling,
    # so it must use the FIELD width and read at addr + BytePosition. (An interior
    # read NAKs fail-soft on a firmware that doesn't honour it -- safer than
    # reading the whole block and mis-decoding the neighbouring bytes, which is
    # what using BlockLength here used to do, e.g. 0xA305 burner load.)
    block_len = ev.block_length or ev.byte_length or 1
    length = ev.byte_length or ev.block_length or 1
    field_off = ev.byte_position or 0
    field_addr = addr + field_off
    addr_line = f"  address: 0x{field_addr:04X}"
    if field_off:
        addr_line += f"  # byte {field_off} of the {block_len}-byte block @ 0x{addr:04X}; verify on hardware"
    enum_opts = _enum_options(ev)
    is_bit = (ev.bit_length or 0) > 0
    writable = _is_writable(ev)
    # A writable datapoint whose converter has no encode path (sec2hour,
    # rotatebytes are read-only in the component -- number.py only accepts
    # encodable converters) must not be emitted as a `number`: the generated
    # config would fail `esphome config`. Demote it to a read-only sensor.
    if writable and conv in ("sec2hour", "rotatebytes"):
        writable = False
        note = (note + "; " if note else "") + f"converter {conv!r} is read-only -> demoted to sensor"

    # Byte-array-as-string (HexByte2AsciiByte): emit the ascii text_sensor type
    # (Sachnummer, Herstellnummer, ...). Read-only device identity; the field
    # can exceed the 4-byte scalar limit (the Herstellnummer is 16 bytes), so
    # this must precede the >4-byte comment fallback below.
    if ev.conversion == "HexByte2AsciiByte":
        if length > 32:
            return (
                "comment",
                [f"# {name} @ 0x{addr:04X}: HexByte2AsciiByte length {length} > 32 -> custom decode"],
            )
        return (
            "text_sensor",
            [
                "- platform: vitohome",
                "  type: ascii",
                f"  name: {_yaml_str(name)}",
                addr_line,
                f"  length: {length}",
                "  entity_category: diagnostic",
            ],
        )

    # UTF-16LE label (HexByte2UTF16Byte): editable heating-circuit names
    # Beschriftung_HK1..3 (40 bytes = 20 chars). Emitted read-only here (display
    # of the label); round-trip editing would need a `text:` platform with a
    # UTF-16 encode path. Must precede the >4-byte comment fallback.
    if ev.conversion == "HexByte2UTF16Byte":
        if length > 40 or (length % 2) != 0:
            return (
                "comment",
                [f"# {name} @ 0x{addr:04X}: HexByte2UTF16Byte length {length} (>40 or odd) -> custom decode"],
            )
        return (
            "text_sensor",
            [
                "- platform: vitohome",
                "  type: utf16",
                f"  name: {_yaml_str(name)}",
                addr_line,
                f"  length: {length}",
            ],
        )

    # A field wider than 4 bytes can't go through the 1..4-byte scalar converters.
    if length > 4 and not is_bit and not enum_opts:
        return (
            "comment",
            [f"# {name} @ 0x{addr:04X}: {ev.conversion or 'raw'} length {length} > 4 bytes -> custom decode"],
        )
    # Scalar reads are clamped to the converter-supported 1..4 bytes.
    num_length = length if length in (1, 2, 3, 4) else max(1, min(4, length))

    poll = _poll_for(ev, conv_kind)
    lines: list[str] = []
    if note:
        lines.append(f"  # NOTE: {note}")

    # --- writable: select (enum) or number ---
    # A writable enum becomes a select when the field is 1-2 bytes AND every
    # option value fits that width. 1-2 is the range components/vitohome/select.py
    # accepts (validate_length_in(1, 2)); the per-value fit check mirrors that
    # file's _validate_options (raw_fits(value, length, is_signed=False)), so a
    # select this emits will not be rejected at `esphome config` time. Wider or
    # non-fitting enums fall through to the number branch below.
    # A semantic on/off pair becomes a switch instead of a two-option select:
    # HA then gets a native toggle (switch.turn_on/off, voice assistants,
    # binary automation conditions). _boolean_pair is deliberately
    # conservative -- non-boolean two-option enums (Celsius/Fahrenheit,
    # Einkessel/Mehrkessel) keep falling through to the select branch.
    bool_pair = _boolean_pair(enum_opts) if enum_opts else None
    if writable and bool_pair and length in (1, 2) and all(0 <= v < (1 << (8 * length)) for v, _ in enum_opts):
        on_v, off_v, on_l, off_l = bool_pair
        lines += [
            "- platform: vitohome",
            f"  name: {_yaml_str(name)}",
            addr_line,
            f"  length: {length}",
        ]
        state_addr = COMMAND_STATE_ADDR.get(ev.address)
        if state_addr is not None:
            lines.append(
                f"  state_address: 0x{state_addr:04X}  # live state read here; address above is the write/command target"
            )
        if (on_v, off_v) != (1, 0):
            lines.append(f"  on_value: 0x{on_v:0{2 * length}X}  # {on_l}")
            lines.append(f"  off_value: 0x{off_v:0{2 * length}X}  # {off_l}")
        else:
            lines.append(f"  # on 0x{on_v:02X} = {on_l}; off 0x{off_v:02X} = {off_l}")
        lines += [
            "  disabled_by_default: true",
            f"  update_interval: {poll}s",
        ]
        return ("switch", lines)

    if writable and enum_opts and length in (1, 2) and all(0 <= v < (1 << (8 * length)) for v, _ in enum_opts):
        lines += [
            "- platform: vitohome",
            f"  name: {_yaml_str(name)}",
            addr_line,
            f"  length: {length}",
        ]
        state_addr = COMMAND_STATE_ADDR.get(ev.address)
        if state_addr is not None:
            lines.append(
                f"  state_address: 0x{state_addr:04X}  # live state read here; address above is the write/command target"
            )
        lines += [
            "  disabled_by_default: true",
            f"  update_interval: {poll}s",
            "  options:",
        ]
        for v, label in enum_opts:
            lines.append(f"    0x{v:0{2 * length}X}: {_yaml_str(label)}")
        return ("select", lines)

    if writable:
        lo = ev.lower or (ev.values[0].lower if ev.values else "")
        hi = ev.upper or (ev.values[0].upper if ev.values else "")
        step = ev.stepping or (ev.values[0].stepping if ev.values else "")
        lines += [
            "- platform: vitohome",
            f"  name: {_yaml_str(name)}",
            addr_line,
            f"  length: {length}",
            f"  converter: {conv}",
        ]
        # A negative lower border means the raw byte(s) are two's-complement.
        # div2/div10 are already signed in the component; noconv is unsigned by
        # default, so it needs an explicit signed: true to decode/accept a
        # negative bound (otherwise the component's number validator rejects it).
        try:
            _lo_neg = lo != "" and float(lo) < 0
        except (TypeError, ValueError):
            _lo_neg = False
        is_signed = conv in ("div2", "div10") or (conv == "noconv" and _lo_neg)
        if conv == "noconv" and _lo_neg:
            lines.append("  signed: true")
        unit = _unit_for(ev)
        if unit:
            lines.append(f"  unit_of_measurement: {_yaml_str(unit)}")
        dc = _device_class_for(unit)
        if dc:
            lines.append(f"  device_class: {dc}")
        have_bounds = (
            lo != ""
            and hi != ""
            and step not in ("", "0")
            and _bound_fits(lo, length, conv, is_signed)
            and _bound_fits(hi, length, conv, is_signed)
        )
        if have_bounds:
            lines.append(f"  min_value: {lo}")
            lines.append(f"  max_value: {hi}")
            lines.append(f"  step: {step}")
        else:
            lines.append("  # Borders absent, or out of range for this byte width, in")
            lines.append("  # the export. These placeholders pass `esphome config` but")
            lines.append("  # pin the value to 0 - set real min/max/step before use.")
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
        if block_len > 4 or byte_off > 3 or byte_off >= block_len:
            return (
                "comment",
                [
                    f"# {name} @ 0x{addr:04X}: bit {bit_pos} of a {block_len}-byte block "
                    f"(byte {byte_off}) exceeds binary_sensor length/offset limits -> custom handling"
                ],
            )
        lines += [
            "- platform: vitohome",
            f"  name: {_yaml_str(name)}",
            f"  address: 0x{addr:04X}",
            f"  length: {block_len}",
        ]
        if byte_off:
            lines.append(f"  byte_offset: {byte_off}")
        lines.append(f"  bit_mask: 0x{mask:02X}")
        lines.append("  disabled_by_default: true")
        lines.append(f"  update_interval: {poll}s")
        return ("binary_sensor", lines)

    if enum_opts:
        enum_len = _enum_read_length(enum_opts, num_length)
        lines += [
            "- platform: vitohome",
            "  type: enum",
            f"  name: {_yaml_str(name)}",
            addr_line,
            f"  length: {enum_len}",
            "  disabled_by_default: true",
            f"  update_interval: {poll}s",
            "  options:",
        ]
        for v, label in enum_opts:
            lines.append(f"    0x{v:0{2 * enum_len}X}: {_yaml_str(label)}")
        return ("text_sensor", lines)

    # numeric sensor
    signed = conv in ("div2", "div10")
    lines += [
        "- platform: vitohome",
        f"  name: {_yaml_str(name)}",
        addr_line,
        f"  length: {length}",
        f"  converter: {conv}",
    ]
    if signed and conv == "noconv":
        lines.append("  signed: true")
    unit = _unit_for(ev)
    if unit:
        lines.append(f"  unit_of_measurement: {_yaml_str(unit)}")
    dc = _device_class_for(unit)
    if dc:
        lines.append(f"  device_class: {dc}")
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
    """An error-history slot: the generic ecnsysEventType~Error block, OR a
    device-specific FehlerHis* slot, OR the hardcoded 0x7507 fallback. The
    'Index'/pointer slot is excluded.

    NOTE: these are TWO DIFFERENT archives, not two addresses for one. On
    VScotHO1_72 the export carries BOTH: ecnsysEventType~Error at 0x7507
    (length 90 = the 10-slot x 9-byte Vitotronic SYSTEM fault history -- the
    one the Bedienteil shows and vcontrold's getError0..9 read;
    hardware-confirmed correct) AND FehlerHisFA01..20 at 0x7590..0x763B --
    the Feuerungsautomat (GFA burner control unit) history, a different
    subsystem with a different code space (GFA_Kennung sits at 0x7650, K38
    is KonfiFehlerByteGFA). An earlier revision falsely assumed this unit
    had no 0x7507 event and crowned FA01 "Letzter Fehler"; on hardware that
    read the wrong archive. See _error_history_entries for the emission
    rules."""
    if ev.address is None:
        return False
    tok = ev.token or ev.tech or ev.name or ""
    if "Index" in tok or "Zeiger" in tok:
        return False
    if "ecnsysEventType" in tok and "Error" in tok:
        return True
    if re.search(r"FehlerHis", tok, re.I):
        return True
    return ev.address == _ERROR_HISTORY_ADDRESS


_FEHLERHIS_SLOT_RE = re.compile(r"FehlerHis\w*?(\d{1,2})\b", re.I)


def _error_history_slot(ev: Event) -> int | None:
    """Slot number for a FehlerHis* token (FehlerHisFA01 -> 1), else None."""
    m = _FEHLERHIS_SLOT_RE.search(ev.token or ev.tech or ev.name or "")
    return int(m.group(1)) if m else None


def _error_history_entries(ev: Event) -> list[dict]:
    """Expand one error-history event into per-slot emission specs.

    Returns dicts with: address, name, seed, disabled, system (bool),
    comment (list of lines). Rules:
    - FehlerHisFA* -> ONE slot of the Feuerungsautomat (GFA burner control
      unit) history: named "GFA Fehler NN", ALL disabled by default, and
      flagged system=False so the Vitotronic fault-code map is NOT attached
      (the GFA lockout codes are a different code space).
    - ecnsysEventType~Error with a 90-byte block -> TEN system slots at
      base + i*9 (vcontrold getError0..9, "Ermittle Fehlerhistory Eintrag
      1..10"); slot 1 is "Letzter Fehler" (newest first --
      hardware-confirmed on VScotHO1_72 @ 0x7507), slots 2..10 disabled.
    - any other system-history event (single slot, e.g. the 0x7507 address
      fallback) -> one enabled "Letzter Fehler" slot at its own address.
    """
    fa_slot = _error_history_slot(ev)
    if fa_slot is not None:
        return [
            {
                "address": ev.address,
                "name": f"GFA Fehler {fa_slot:02d}",
                "seed": f"gfa_fehler_{fa_slot:02d}",
                "disabled": True,
                "system": False,
                "comment": [
                    f"# Feuerungsautomat (GFA burner control unit) history slot {fa_slot}:",
                    "# a DIFFERENT archive from the Vitotronic system history",
                    "# (HARDWARE-CONFIRMED distinct on VScotHO1_72: slot 1 read 0x00 with",
                    "# its own timestamp 42 s after the system slot's 0x38). GFA lockout",
                    "# codes are a different code space from the F-codes, so no codes map",
                    "# is attached and the code byte displays as raw hex. No public GFA",
                    "# code enumeration is known (export + community search both empty);",
                    "# a populated slot from a real burner lockout would seed one.",
                ],
            }
        ]
    blen = ev.byte_length or ev.block_length or 9
    if blen >= 90:
        entries = []
        for i in range(10):
            slot = i + 1
            entries.append(
                {
                    "address": (ev.address or 0) + i * 9,
                    "name": "Letzter Fehler" if slot == 1 else f"Fehler {slot:02d}",
                    "seed": "letzter_fehler" if slot == 1 else f"fehler_{slot:02d}",
                    "disabled": slot > 1,
                    "system": True,
                    "comment": [
                        f"# System fault history slot {slot} of 10 (ecnsysEventType~Error,",
                        f"# 90-byte block at 0x{ev.address:04X} + {i}*9; vcontrold getError{i}).",
                    ]
                    + (["# Slot 1 = newest entry -- hardware-confirmed on VScotHO1_72."] if slot == 1 else []),
                }
            )
        return entries
    return [
        {
            "address": ev.address,
            "name": "Letzter Fehler",
            "seed": "letzter_fehler",
            "disabled": False,
            "system": True,
            "comment": [
                "# System fault history, most-recent slot (code byte + 8-byte BCD",
                "# timestamp).",
            ],
        }
    ]


def _is_mode_select(ev: Event) -> bool:
    return ev.access_type in (2, 3) and bool(
        re.search(r"Betriebsart|BedienteilBA|BedienBetriebsart", ev.token or ev.tech or "", re.I)
    )


# Fault / status / health registers. Tagged entity_category: diagnostic so they
# group under Diagnostics in Home Assistant rather than mixing with measurements.
# Covers: fault bytes (GFA 0x5738, EEPROM/I2C), the LON alarm record (0xA132),
# sensor-status enums (TemperaturFehler_*, *Sensor*Status*, 0x089C.., 0xCF90),
# collective-fault flags (Sammel*), and the system/maintenance status enums.
_DIAGNOSTIC_RE = re.compile(
    r"Fehler|Stoer|St[oö]r|Status|Sensor.*Status|SensorStatus|Alarm|EEPROM|I2C|"
    r"Diagnos|Sammel|Wartung|Quitt|TemperaturFehler|ecnStatusEventType",
    re.I,
)


def _is_diagnostic(ev: Event) -> bool:
    return bool(_DIAGNOSTIC_RE.search(ev.token or ev.tech or ev.name or ""))


def _inject_diagnostic(lines: list[str]) -> list[str]:
    """Add entity_category: diagnostic once, if not already present."""
    if any(ln.strip().startswith("entity_category:") for ln in lines):
        return lines
    for idx, ln in enumerate(lines):
        if ln.startswith("  name:"):
            lines.insert(idx + 1, "  entity_category: diagnostic")
            return lines
    return lines


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


def _error_history_lines(entry: dict, oid: str, codes_map: dict | None, set_name: str = "") -> list[str]:
    """One error-history slot (from _error_history_entries) as a 9-byte block
    (code byte + 8-byte BCD timestamp), decoded by the component's
    error_history type. ``codes_map`` is attached only to SYSTEM slots -- the
    GFA archive is a different code space and renders raw hex instead."""
    lines = list(entry["comment"])
    lines += [
        "# If the device NAKs the 9-byte block read, drop length to 1 (code only).",
        "- platform: vitohome",
        "  type: error_history",
        f"  name: {_yaml_str(entry['name'])}",
        f"  id: {oid}",
        f"  address: 0x{entry['address']:04X}",
        "  length: 9",
        '  icon: "mdi:alert-circle"',
        "  entity_category: diagnostic",
    ]
    if entry["disabled"]:
        lines.append("  disabled_by_default: true")
    lines.append(f"  update_interval: {POLL_ERROR}s")
    if entry["system"] and codes_map:
        label = f"'{set_name}' set" if set_name else "set"
        lines.append(f"  # Fault-code map ({label}) - DEFAULT, verify for this unit (see fault_codes.CONFLICTS):")
        lines.append("  codes:")
        for code, text in sorted(codes_map.items()):
            lines.append(f"    0x{code:02X}: {_yaml_str(text)}")
    elif entry["system"]:
        lines.append('  # codes: { 0x00: "kein Fehler", ... }  # add a code->text map')
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


# The only read function code VitoWiFi's standard datapoint read issues over
# Optolink is Virtual_READ (VS2/P300 FunctionCode 0x01). Datapoints whose access
# method is anything else -- GFA_READ (0x6B), Remote_Procedure_Call (0x07),
# PROZESS_READ (0x7B), KBUS_*/OT_*/Physical_READ -- are not reachable through that
# read: the device NAKs or returns unrelated virtual-space data. They are dropped
# by default (--no-reachable-only keeps them). An empty FCRead (no access row)
# means "unknown" and is kept, since the token-derived address is treated as a
# normal virtual read. Refs: openv "Protokoll 300";
# InsideViessmannVitosoft/VitosoftCommunication.md.
def _is_reachable(ev: Event) -> bool:
    return (ev.fc_read or "").strip().lower() in ("", "virtual_read")


# Mirror of _is_reachable on the write side. A datapoint Vitosoft marks writable
# (access type 2/3) is only actually settable over Optolink when its write
# function code is Virtual_WRITE -- the only write VitoWiFi issues. FCWrite
# "undefined", GFA_WRITE, Remote_Procedure_Call, etc. mean the write would not
# take, so the datapoint is demoted to a read-only sensor instead of a
# number/select. Blank FCWrite is unknown and trusted to the access type.
def _is_writable(ev: Event) -> bool:
    if ev.access_type not in (2, 3):
        return False
    return (ev.fc_write or "").strip().lower() in ("", "virtual_write")


def generate(
    catalog: Catalog,
    device: str,
    profile: str,
    include_re: str | None,
    exclude_re: str | None,
    emit_device_id: bool = True,
    emit_error_history: bool = True,
    error_codes: bool = True,
    error_code_set: str = "vd300",
    reachable_only: bool = True,
) -> str:
    events = catalog.events_for(device)
    if not events:
        raise SystemExit(
            f"device {device!r} not found or has no events. Run with --list-devices to see available device tokens."
        )

    # When the device defines its own per-slot fault log (FehlerHis*), those are
    # authoritative; suppress the generic ecnsysEventType~Error / 0x7507 slot so
    # we don't emit a duplicate "Letzter Fehler" (and a possibly-phantom 0x7507).

    # Resolve the fault-code map once: the chosen set, or None when codes are off.
    codes_map = fault_codes.SETS.get(error_code_set) if error_codes else None
    codes_set_name = error_code_set if error_codes else ""

    inc = re.compile(include_re) if include_re else None
    exc = re.compile(exclude_re) if exclude_re else None

    buckets: dict[str, list[str]] = {
        "sensor": [],
        "binary_sensor": [],
        "number": [],
        "select": [],
        "switch": [],
        "text_sensor": [],
    }
    comments: list[str] = []

    # De-duplication identity. Keying on the base address alone silently
    # collapsed DISTINCT fields carved from one block: verified against the
    # full 2026 export, VScotHO1_72 has 43 base addresses carrying more than
    # one field at different BytePositions (HWIndex_SM1/SWIndex_SM1 at 0x0A68,
    # the RF_TeilnehmerInfo_* families, RF_AllgemeinInfo_* at 0x0D30, ...),
    # and only the first survived. A datapoint's identity is the FIELD --
    # (address, byte_position, bit_position) -- which still folds true
    # duplicates (same field via several event variants) into one entity.
    def _field_key(ev: Event):
        return (ev.address, ev.byte_position or 0, ev.bit_position or 0)

    seen_addr: set = set()
    used_ids: set[str] = set()
    kept = 0
    dropped_unreachable = 0

    # Hub-fed device identity (covers the 0xF8..0xFB identification reads, which
    # are then suppressed below).
    if emit_device_id:
        oid = _make_obj_id("device_type", used_ids)
        buckets["text_sensor"].extend("  " + ln if ln else "" for ln in _device_id_lines(oid))
        kept += 1

    for ev in sorted(events, key=lambda e: e.address or 0):
        # Special datapoints are handled independently of profile/name filters.
        if emit_device_id and _is_identification(ev):
            continue  # represented by the device_id entity above
        if emit_error_history and _is_error_history(ev):
            # The SYSTEM history (ecnsysEventType~Error / the 0x7507 fallback)
            # and the GFA (Feuerungsautomat) FehlerHis* archive are different
            # subsystems and BOTH emit -- the system one owns "Letzter Fehler"
            # and the fault-code map; GFA slots are named, disabled, and
            # code-map-free. A 90-byte system block expands into its 10 slots.
            for entry in _error_history_entries(ev):
                if entry["address"] is None:
                    continue
                key = (entry["address"], 0, 0)
                if key in seen_addr:
                    continue
                seen_addr.add(key)
                oid = _make_obj_id(entry["seed"], used_ids)
                buckets["text_sensor"].extend(
                    "  " + ln if ln else "" for ln in _error_history_lines(entry, oid, codes_map, codes_set_name)
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
        if ev.address is not None and _field_key(ev) in seen_addr:
            continue

        if reachable_only and not _is_reachable(ev):
            dropped_unreachable += 1
            continue

        result = emit_entity(ev, profile)
        if result is None:
            continue
        platform, lines = result
        if platform == "comment":
            comments.extend(lines)
            continue
        if ev.address is not None:
            seen_addr.add(_field_key(ev))
        # Writable operating-mode select: prepend the hardware caveat.
        if platform == "select" and _is_mode_select(ev):
            lines = _mode_select_caveat() + lines
        # Fault / status / health registers -> Diagnostics category.
        if platform in ("sensor", "binary_sensor", "text_sensor") and _is_diagnostic(ev):
            lines = _inject_diagnostic(lines)
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
    if reachable_only:
        out.append(f"# FCRead filter ON: {dropped_unreachable} datapoint(s) requiring a non-Optolink")
        out.append("# access method (GFA_READ/RPC/PROZESS/KBUS/OT) were omitted as unreachable")
        out.append("# via VitoWiFi's read. Re-run with --no-reachable-only to include them.")
        out.append("#")
    out.append("# VERIFY ON HARDWARE: not every address answers on every firmware.")
    out.append("# Run `esphome config` then `esphome compile`/`run` before relying")
    out.append("# on any value. number entities whose borders were absent in the")
    out.append("# export get min/max/step = 0/0/1 placeholders: valid config, but")
    out.append("# pinned to 0 until you fill in the real bounds.")
    out.append("# ============================================================")
    out.append("")

    for platform in ("sensor", "binary_sensor", "number", "select", "switch", "text_sensor"):
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
    p.add_argument(
        "--data", required=True, help="dir with the Vitosoft XML export (DPDefinitions*.xml, ecnEventType.xml, ...)"
    )
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
    p.add_argument(
        "--device-id",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="emit a device_id diagnostic text_sensor and suppress the raw 0xF8..0xFB reads (default: on)",
    )
    p.add_argument(
        "--error-history",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="emit error_history text_sensors for the FehlerHis* slots (or 0x7507 fallback) (default: on)",
    )
    p.add_argument(
        "--error-codes",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="attach a fault-code map (see --error-code-set) to error_history entities (default: on)",
    )
    p.add_argument(
        "--error-code-set",
        choices=sorted(fault_codes.SETS),
        default="vd300",
        help="which fault-code map to attach: vd300 (Vitodens 300-W B3HA = VScotHO1_72, default), "
        "vd200 (Vitodens 200 WB2A), openv (generic), or union (all, most-specific wins); "
        "fault-code semantics are device-variant-specific -- verify on the unit",
    )
    p.add_argument(
        "--reachable-only",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="emit only datapoints VitoWiFi can read over Optolink (FCRead Virtual_READ); "
        "drop GFA_READ/RPC/PROZESS/KBUS/OT (default: on)",
    )
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
        catalog,
        device,
        args.profile,
        args.include,
        args.exclude,
        emit_device_id=args.device_id,
        emit_error_history=args.error_history,
        error_codes=args.error_codes,
        error_code_set=args.error_code_set,
        reachable_only=args.reachable_only,
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
