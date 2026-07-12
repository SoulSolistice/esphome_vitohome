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

    # generate a catalog for EVERY device token at once (one file per unit),
    # named unit_swIndex[_variant].yaml (vscotho1_72.yaml, gwg_vbes_00.yaml, ...)
    # with an index.csv manifest; narrow with --export-filter if wanted:
    python3 scripts/gen_catalog.py --data <export-dir> --export-all --out ./catalogs
    python3 scripts/gen_catalog.py --data <export-dir> --export-all --out ./catalogs \
        --export-filter '^V' --no-error-codes

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
from collections import defaultdict
import csv
from dataclasses import dataclass, field
import math
import os
import re
import sys
import xml.etree.ElementTree as ET

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
    "MultOffsetBCD": "BCD-coded value -> custom decode",
}

# ConversionFactor values that map 1:1 onto an existing component converter
# when ConversionOffset is 0. Used for ``MultOffset`` (whose semantics are
# value = raw * factor + offset) and for the handful of ``NoConversion`` rows
# that carry a real factor (verified in the 2026 export: factors 0.1 and 10 on
# Virtual_READ rows). Factors outside this map fall back to noconv plus an
# ESPHome ``multiply:`` filter on read-only sensors (see emit_entity).
_FACTOR_PRESETS = (
    (2.0, "mult2"),
    (5.0, "mult5"),
    (10.0, "mult10"),
    (100.0, "mult100"),
    (0.5, "div2"),
    (0.1, "div10"),
    (0.01, "div100"),
    (0.001, "div1000"),
)

# Vitosoft's placeholder junk pair: rows with no real conversion factor carry
# the literal strings -0.1067 / 6992.58 (verified across the 2026 export --
# they appear together on Phone2BCD, HexByte2AsciiByte, VitocomNV and other
# non-numeric rows). Treat the pair as "no factor/offset present".
_FACTOR_PLACEHOLDERS = ("-0.1067", "6992.58")

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

# Culture name -> CultureId in Textresource.xml. Mirrors the full ecnCulture
# table of the 2026 export (13 cultures). NOTE: the export ships only Vitosoft
# UI strings in Textresource.xml -- no eventtype / eventvaluetype / ecnUnit
# labels -- so the culture choice currently has no effect on entity naming;
# the map is kept complete for exports that do carry translations.
_CULTURES = {
    "de": "1",
    "en": "2",
    "fr": "3",
    "it": "4",
    "ru": "5",
    "nl": "6",
    "pl": "7",
    "da": "8",
    "hu": "9",
    "es": "10",
    "tr": "11",
    "lt": "12",
    "cs": "13",
}

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
    # Vitosoft access semantics, verified against the full 2026 export (the
    # DPDefinitions ``Type`` column agrees with the access layer's AccessMode
    # on all 11582 rows): 1 = read-only, 2 = WRITE-ONLY (reset/trigger
    # registers), 3 = read+write. NOTE 2/3 are the reverse of what an earlier
    # comment here claimed; every writability check treats {2, 3} as writable,
    # and write-only (2) additionally gets no polled state read (emit_entity).
    access_type: int
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
    # Access-layer value transform: value = raw * conv_factor + conv_offset.
    # None when absent or when the export carries its placeholder junk pair
    # (see _FACTOR_PLACEHOLDERS). Consumed for MultOffset and for identity
    # conversions with a real factor; never stacked on Div*/Mult* presets.
    conv_factor: float | None = None
    conv_offset: float | None = None
    # Access-layer array structure: BlockLength bytes = block_factor records
    # (e.g. ecnsysEventType~Error: BlockLength 90, BlockFactor 10 -> ten 9-byte
    # fault slots; MappingType 3). 0/None when the datapoint is not an array.
    block_factor: int | None = None
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
    hits = [
        os.path.join(dirpath, f)
        for dirpath, _dirs, files in os.walk(data_dir)
        for f in files
        if re.match(r"DPDefinitions.*\.xml$", f, re.IGNORECASE)
    ]
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
    key = raw.removeprefix("@@")
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
            acc[i] = {
                "address": _child(e, "Address"),
                "block_length": _child(e, "BlockLength"),
                "byte_length": _child(e, "ByteLength"),
                "bit_length": _child(e, "BitLength"),
                "bit_position": _child(e, "BitPosition"),
                "byte_position": _child(e, "BytePosition"),
                "conversion": _child(e, "Conversion"),
                "conversion_factor": _child(e, "ConversionFactor"),
                "conversion_offset": _child(e, "ConversionOffset"),
                "block_factor": _child(e, "BlockFactor"),
                "mapping_type": _child(e, "MappingType"),
                "lower_border": _child(e, "LowerBorder"),
                "upper_border": _child(e, "UpperBorder"),
                "stepping": _child(e, "Stepping"),
                "unit": _child(e, "Unit"),
                "fc_read": _child(e, "FCRead"),
                "fc_write": _child(e, "FCWrite"),
            }
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
                {
                    "token": tok,
                    "Identification": ident,
                    "IdentificationExtension": ext,
                    "IdentificationExtensionTill": extt,
                    "ident": _hx(ident),
                    "ext": _hx(ext),
                    "extt": _hx(extt),
                    "f0": _hx(_child(e, "F0")),
                    "f0t": _hx(_child(e, "F0Till")),
                }
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
    "ecnEventTypeGroup",
    "ecnEventTypeEventTypeGroupLink",
}


class Catalog:
    """Parsed Vitosoft export: device types + events (merged with the access
    layer) + value types + identification."""

    def __init__(
        self, types, dp_name, links, raw_events, vlinks, raw_values, access, ident_rows, textmap, groups=None, glinks=None
    ):
        self.devices = types  # token -> datapoint-type Id
        self._dp_name = dp_name  # Id -> display name
        self._links = links  # dp_id -> [event_id]
        self._vlinks = vlinks  # event_id -> [value_id]
        self._access = access  # token -> access dict
        self._ident = ident_rows
        self._text = textmap
        # Vitosoft navigation tree (ecnEventTypeGroup): group id -> {"path":
        # readable "TOKEN~10_Bedienung_A1~20_Warmwasser", "dp": DataPointTypeId,
        # "order": OrderIndex}; glinks: event id -> [(group id, EventTypeOrder)].
        # Consumed only by --order group; both default empty.
        self._groups = groups or {}
        self._glinks = glinks or {}
        self._group_cache: dict = {}
        self._values = {vid: self._mk_value(f) for vid, f in raw_values.items()}
        self._events = {eid: self._mk_event(eid, f) for eid, f in raw_events.items()}

    def group_for(self, ev, device_token: str) -> tuple[tuple, str]:
        """(sort_key, label) of the event's Vitosoft navigation group for this
        device, from ecnEventTypeGroup. The group's Address embeds the full
        readable path ("VScotHO1_72~10_Bedienung_A1~20_Warmwasser"); its
        numeric segment prefixes carry Vitosoft's intended ordering, so the
        raw segments are the sort key (all-str tuple; EventTypeOrder appended
        zero-padded to order entities within a group). Events without a group
        sort last under "(ohne Gruppenzuordnung)"."""
        dp_id = self.devices.get(device_token)
        cache_key = (ev.id, dp_id)
        hit = self._group_cache.get(cache_key)
        if hit is not None:
            return hit
        best = None
        for gid, ev_order in self._glinks.get(ev.id, ()):
            g = self._groups.get(gid)
            if not g or g["dp"] != dp_id:
                continue
            segs = tuple(g["path"].split("~")[1:])
            if not segs:
                continue
            cand = (segs, ev_order)
            if best is None or cand < best:
                best = cand
        if best is None:
            out = (("~ohne_gruppe", "~"), "(ohne Gruppenzuordnung)")  # '~' > alnum -> sorts last
        else:
            segs, ev_order = best
            # "10_Bedienung_A1" -> "Bedienung A1"; consecutive duplicate
            # segments collapse ("03_Identifikation~10_Identifikation").
            clean: list[str] = []
            for seg in segs:
                c = re.sub(r"^\d+_", "", seg).replace("_", " ")
                if not clean or clean[-1] != c:
                    clean.append(c)
            out = (segs + (f"{ev_order:06d}",), " / ".join(clean))
        self._group_cache[cache_key] = out
        return out

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

        def pick_str(acc_key, dp_key):
            """DPDefinitions first (fixture/old exports), else the access layer.
            The 2026 export keeps borders ONLY in the access layer."""
            v = f.get(dp_key, "") or ""
            if v == "":
                v = acc.get(acc_key) or ""
            return v

        raw_name = f.get("Name", "") or ""
        # Explicit None checks: 0x0000 is a valid Optolink address, and a
        # falsy-zero `or` chain would silently treat it as absent.
        addr = _hexaddr(acc.get("address"))
        if addr is None:
            addr = _address_from_token(token)
        if addr is None:
            addr = _address_from_name(raw_name)

        # value = raw * factor + offset. The export writes the junk pair
        # -0.1067 / 6992.58 on rows with no real transform; and ConversionFactor
        # 0 is the dominant "no scaling" sentinel (4364 NoConversion rows in the
        # 2026 export) -- a literal 0x multiplier is never meaningful, so treat
        # both as absent. A real 0 offset is fine and handled at the call site.
        def _fval(acc_key, dp_key, *, is_factor):
            s = f.get(dp_key, "") or acc.get(acc_key) or ""
            if s in _FACTOR_PLACEHOLDERS:
                return None
            try:
                v = float(s)
            except (TypeError, ValueError):
                return None
            if is_factor and v == 0.0:
                return None
            return v

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
            lower=pick_str("lower_border", "LowerBorder"),
            upper=pick_str("upper_border", "UpperBorder"),
            stepping=pick_str("stepping", "Stepping"),
            enum_type=(f.get("EnumType", "") or "").lower() in ("1", "true"),
            unit=_resolve_unit(acc.get("unit", ""), self._text),
            tech=_tech_id(raw_name, token),
            token=token,
            fc_read=(acc.get("fc_read") or f.get("FCRead") or ""),
            fc_write=(acc.get("fc_write") or f.get("FCWrite") or ""),
            conv_factor=_fval("conversion_factor", "ConversionFactor", is_factor=True),
            conv_offset=_fval("conversion_offset", "ConversionOffset", is_factor=False),
            block_factor=pick("block_factor", "BlockFactor"),
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
    groups: dict = {}
    glinks = defaultdict(list)

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
            elif ln == "ecnEventTypeGroup":
                gid = f.get("Id")
                if gid:
                    groups[gid] = {
                        "path": f.get("Address") or "",  # readable "TOKEN~10_Bedienung~..."
                        "dp": f.get("DataPointTypeId") or "",
                        "order": _intval(f.get("OrderIndex")) or 0,
                    }
            elif ln == "ecnEventTypeEventTypeGroupLink":
                e, gid = f.get("EventTypeId"), f.get("EventTypeGroupId")
                if e and gid:
                    glinks[e].append((gid, _intval(f.get("EventTypeOrder")) or 0))
        if ln.startswith(_CONTAINER_PREFIXES):
            el.clear()  # free table rows / datasets we have finished reading

    return Catalog(
        types, dp_name, links, raw_events, vlinks, raw_values, access, ident, textmap, groups=groups, glinks=glinks
    )


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
    seen_values: set[int] = set()
    for v in ev.values:
        if v.enum_address_value is None:
            continue
        if v.enum_address_value < 0:
            # A handful of value types (SNVTAlarm_*, WPR3_Mischer_*) carry
            # EnumAddressValue -1. The raw bytes are compared unsigned, so a
            # negative option can never match on the wire -- and emitting it
            # produced invalid `0x-1` option keys that fail `esphome config`
            # (verified on the CU401B/V333 bulk exports). Skipped; emit_entity
            # adds a NOTE naming how many were dropped.
            continue
        if v.enum_address_value in seen_values:
            # The export can map TWO options to the SAME value (LON/BACnet
            # nodes: nvoNodeAlarm at 0x82E has value 3 for both
            # "service alarm 2" and "service alarm 3"). Duplicate values become
            # duplicate YAML option keys, which fail `esphome config`. Keep the
            # first (deterministic by document order) and drop the rest.
            continue
        seen_values.add(v.enum_address_value)
        label = v.description or v.enum_replace_value or v.name or f"0x{v.enum_address_value:02X}"
        out.append((v.enum_address_value, label))
    return out


def _negative_option_count(ev: Event) -> int:
    """How many enum options _enum_options dropped for being negative."""
    return sum(1 for v in ev.values if v.enum_address_value is not None and v.enum_address_value < 0)


def _dedup_option_labels(opts: list) -> list:
    """ESPHome `select` (and our enum text_sensor for consistency) require
    UNIQUE option labels, but the export can map several values to the same
    string -- e.g. KF1_KonfiTemperaturprogramm has values 6..15 all labelled
    "Default" (untranslated placeholders). Disambiguate a repeated label by
    appending its raw value ("Default (0x06)"); first occurrence is left as-is
    so the common case stays clean. Order and values are preserved.
    """
    seen: dict[str, int] = {}
    result = []
    for value, label in opts:
        if label in seen and seen[label] != value:
            result.append((value, f"{label} (0x{value:02X})"))
        else:
            seen[label] = value
            result.append((value, label))
    return result


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

# Mirrors CONVERTERS[...].lengths in components/vitohome/__init__.py: the byte
# widths each converter can decode. sensor.py/number.py REJECT a config whose
# (converter, effective width) is outside this table, so the generator must
# never emit such a pair -- verified failing shapes from the bulk export:
# div10 at 4 bytes (HK_Aufheiztimer*), sec2hour at 1 byte (PartyTimer).
# Unsupported combinations fall back to noconv plus a `multiply:` filter
# carrying the true scale (read-only), or are demoted first (writables).
_CONV_LENGTHS = {
    "noconv": (1, 2, 3, 4),
    "div2": (1, 2),
    "div10": (1, 2),
    "div100": (1, 2, 4),
    "div1000": (2, 4),
    "sec2hour": (4,),
    "mult2": (1, 2, 4),
    "mult5": (1, 2, 4),
    "mult10": (1, 2, 4),
    "mult100": (1, 2, 4),
    "rotatebytes": (2,),
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


# Single-telegram READ payload cap for P300/VS2 (mirror of
# components/vitohome/__init__.py::MAX_P300_READ_LENGTH). A block read wider
# than this NAKs on P300 (hardware-observed: 40-byte read at 0x7362), so the
# generator emits a commented hint instead of config that errors on the wire.
# Mirrors components/vitohome/__init__.py::MAX_P300_READ_LENGTH. A 42-byte read
# is hardware-proven on P300 (0x7360, 2026-07-10); 48 is the ceiling we attempt.
# The old 37 was traced to a misread telegram length byte -- see that file.
MAX_P300_READ_LENGTH = 48
MAX_TEXT_BLOCK_LENGTH = MAX_P300_READ_LENGTH


def _string_lines(name: str, kind: str, addr: int, field_len: int, field_off: int, block_len: int) -> list[str] | None:
    """Emit an ascii/utf16 text_sensor.

    A string field at BytePosition > 0 must be read as an ALIGNED BLOCK at the
    block base plus byte_offset. Adding the offset to the address instead
    fabricates a datapoint that does not exist: `Beschriftung_HK1~0x7360`
    (BlockLength 42, BytePosition 2) became `0x7362`, which P300 answers with an
    error telegram at any read width and KW answers with 0xFF fill
    (hardware-confirmed 2026-07-10). Returns None if the block read would exceed
    MAX_TEXT_BLOCK_LENGTH, so the caller can emit a comment rather than a wrong
    entity.
    """
    lines = [
        "- platform: vitohome",
        f"  type: {kind}",
        f"  name: {_yaml_str(name)}",
    ]
    if not field_off:
        lines.append(f"  address: 0x{addr:04X}")
        lines.append(f"  length: {field_len}")
        return lines
    read_len = max(block_len, field_off + field_len)
    if read_len > MAX_TEXT_BLOCK_LENGTH:
        return None
    lines.append(f"  address: 0x{addr:04X}")
    lines.append(f"  length: {read_len}")
    lines.append(f"  byte_offset: {field_off}")
    lines.append(f"  byte_length: {field_len}")
    return lines


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

    # WRITE-ONLY registers (Vitosoft access type 2: FunktionReset triggers,
    # counter resets, the hydraulic-balance byte blob). There is no state to
    # poll -- reading them returns garbage or NAKs -- and a number/switch card
    # would show a meaningless value, so they are surfaced as a hint instead
    # of a polled entity. (The COMMAND_STATE_ADDR command registers all carry
    # access type 3 in the export -- verified -- so this cannot shadow them;
    # the guard is belt-and-braces.)
    if ev.access_type == 2 and COMMAND_STATE_ADDR.get(ev.address) is None:
        return (
            "comment",
            [
                f"# {name} @ 0x{addr:04X}: WRITE-ONLY register (Vitosoft AccessMode 'Write');"
                " a trigger with no read-back -> not auto-emitted."
            ],
        )

    conv, conv_kind = CONVERSION_MAP.get(ev.conversion, (None, PLAIN))
    note = None
    # Value-transform filters (multiply/offset) for READ-ONLY sensors whose
    # factor cannot be expressed as a component converter. Writables never get
    # filters (there is no encode-side transform); they keep raw noconv + note.
    value_filters: list[tuple[str, float]] = []
    force_signed = False
    writable = _is_writable(ev)

    # ConversionFactor/ConversionOffset (value = raw * factor + offset).
    # Applied ONLY for MultOffset and for identity conversions carrying a real
    # factor -- never stacked on Div*/Mult*/Sec2Hour presets (the export
    # sometimes echoes the preset's own factor there; verified: Div2 rows with
    # factor 0.5). Factor -> preset where possible (K90 MultOffset factor 10
    # -> mult10: previously emitted as raw noconv, a 10x under-read).
    # A factor of exactly 0 is the export's "no scaling" sentinel (the loader
    # already maps it to None; this normalises Events built by other means too)
    # -- never a literal 0x multiplier.
    eff_factor = ev.conv_factor if ev.conv_factor not in (None, 0.0) else None
    factor_driven = ev.conversion == "MultOffset" or (ev.conversion in ("", "NoConversion") and eff_factor is not None)
    if factor_driven:
        f_val = eff_factor
        o_val = ev.conv_offset or 0.0
        preset = None
        if f_val is not None and o_val == 0.0 and f_val != 1.0:
            for pf, pname in _FACTOR_PRESETS:
                if abs(f_val - pf) < 1e-12:
                    preset = pname
                    break
        if preset:
            conv = preset
            conv_kind = DIV if preset.startswith("div") else PLAIN
            note = f"conversion {ev.conversion!r} (factor {f_val:g}) -> {preset}"
        elif f_val is not None and (f_val != 1.0 or o_val != 0.0):
            conv = "noconv"
            if writable:
                # No encode-side transform exists; the raw register value is
                # what gets written. Surface the display transform in the note.
                note = (
                    f"conversion {ev.conversion!r}: display value = raw * {f_val:g}"
                    + (f" + {o_val:g}" if o_val else "")
                    + "; raw noconv emitted (no encode path for the transform)"
                )
            else:
                if f_val != 1.0:
                    value_filters.append(("multiply", f_val))
                if o_val:
                    value_filters.append(("offset", o_val))
                note = f"conversion {ev.conversion!r} -> noconv + filter (value = raw * {f_val:g}" + (
                    f" + {o_val:g})" if o_val else ")"
                )
        elif conv is None:
            conv = "noconv"
            note = f"conversion {ev.conversion!r} has no preset; raw noconv emitted"
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
    # Alignment for P300: a scalar field is carved from a block at `addr`.
    # KW (byte-oriented) tolerates an interior read at addr+offset, but P300
    # NAKs an unaligned single-byte read (hardware-confirmed on the pump-speed
    # bytes 0x7661/0x7664). So for a SINGLE-byte field at a non-zero offset,
    # emit an aligned BLOCK read at the base with `byte_offset` -- the
    # component fetches the whole block and extracts the one byte, which is
    # portable across both protocols. Multi-byte interior fields (rare) can't
    # be a single-byte extract; they keep the interior address with the old
    # caveat. Both are gated by the P300 read-length cap below.
    field_width = ev.byte_length or block_len
    # Align to the block base for ANY field (1..4 bytes wide) at a non-zero
    # offset whose block fits the P300 single-telegram cap: fetch the whole
    # block, extract the field via byte_offset (+ byte_length for a multi-byte
    # field). Portable across KW and P300. Fields wider than 4 bytes, or in a
    # block over the cap, can't be a scalar extract -> interior read w/ caveat.
    use_block_extract = field_off > 0 and 1 <= field_width <= 4 and block_len <= MAX_P300_READ_LENGTH
    # The string types carry their own block/offset form and are handled in their
    # own branch below, so they are exempt from the invariant check here.
    is_string = ev.conversion in ("HexByte2AsciiByte", "HexByte2UTF16Byte")
    if field_off and not use_block_extract and not is_string:
        # INVARIANT: never emit `addr + BytePosition`. That address need not
        # exist. P300 answers an unaligned interior read with an error telegram
        # at ANY width -- hardware-confirmed on 0x7362, which is
        # Beschriftung_HK1~0x7360 + BytePosition 2 -- and KW answers it with 0xFF
        # fill, which decodes to a plausible-looking empty value. The old
        # fallback shipped 0x6584 / 0x6587 (bytes 4 and 7 of the 48-byte block at
        # 0x6580) under the label "interior read; P300 may NAK".
        return (
            "comment",
            [
                f"# {name} @ 0x{addr:04X}: {field_width}-byte field at BytePosition {field_off} of a "
                f"{block_len}-byte block -- cannot be expressed as an aligned block read "
                f"(block > {MAX_P300_READ_LENGTH} bytes, or field > 4 bytes)"
            ],
        )
    addr_line = f"  address: 0x{addr:04X}"
    # Interior fallback form: the field's own address, with the P300 caveat.
    # Used whenever a branch cannot express the aligned block extraction
    # (block over the cap, field too wide, or a COMMAND_STATE_ADDR conflict on
    # the writable branches). addr_line above is the block BASE when
    # use_block_extract is set and is only correct together with byte_offset
    # -- using it without byte_offset reads the wrong bytes.
    # There is no "interior fallback" any more. Every branch below is reached with
    # either field_off == 0 (the address IS the field) or an aligned block extract
    # (the address is the block base, paired with byte_offset). Anything else
    # returned a comment above, or was demoted to read-only.
    interior_addr_line = f"  address: 0x{addr:04X}"
    enum_opts = _dedup_option_labels(_enum_options(ev))
    neg_opts = _negative_option_count(ev)
    is_bit = (ev.bit_length or 0) > 0
    # `writable` was resolved above (before conversion handling); demotions
    # below narrow it further.
    # A writable datapoint whose converter has no encode path (sec2hour,
    # rotatebytes are read-only in the component -- number.py only accepts
    # encodable converters) must not be emitted as a `number`: the generated
    # config would fail `esphome config`. Demote it to a read-only sensor.
    if writable and conv in ("sec2hour", "rotatebytes"):
        writable = False
        note = (note + "; " if note else "") + f"converter {conv!r} is read-only -> demoted to sensor"
    # A writable whose display value needs a multiply/offset filter has no
    # encode-side transform either; keep it read-only so the filtered value is
    # at least correct. (Filters were only collected for non-writables above,
    # so this triggers only after a demotion path re-routes -- defensive.)
    if writable and value_filters:
        writable = False
        note = (note + "; " if note else "") + "value transform has no encode path -> demoted to sensor"
    # A writable field at BytePosition > 0 has NO KNOWN WRITE ADDRESS. Reading it
    # is fine (aligned block read at the base + byte_offset), but the write target
    # used to be emitted as `addr + BytePosition`. The export declares that
    # address for only 144 of the 360 such fields, and where it IS declared it
    # generally belongs to an unrelated datapoint of another device family.
    # 0x7362 proves such an address need not exist at all.
    #
    # A read of a non-existent address is a NAK. A WRITE to a wrong-but-existing
    # address changes something. Demote rather than invent a write target.
    if writable and field_off > 0:
        writable = False
        note = (note + "; " if note else "") + (
            f"field at BytePosition {field_off} has no declared write address -> demoted to read-only"
        )

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
        str_lines = _string_lines(name, "ascii", addr, length, field_off, block_len)
        if str_lines is None:
            return (
                "comment",
                [
                    f"# {name} @ 0x{addr:04X}: ascii field of {length} bytes at BytePosition {field_off} "
                    f"of a {block_len}-byte block -- block read exceeds {MAX_TEXT_BLOCK_LENGTH} bytes"
                ],
            )
        str_lines.append("  entity_category: diagnostic")
        return ("text_sensor", str_lines)

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
        utf_lines = _string_lines(name, "utf16", addr, length, field_off, block_len)
        if utf_lines is None:
            return (
                "comment",
                [
                    f"# {name} @ 0x{addr:04X}: utf16 field of {length} bytes at BytePosition {field_off} "
                    f"of a {block_len}-byte block -- block read exceeds {MAX_TEXT_BLOCK_LENGTH} bytes"
                ],
            )
        if field_off:
            utf_lines.append("  disabled_by_default: true")
            utf_lines.append(f"  # aligned block read: {field_off + length} bytes at the block base. The widest read")
            utf_lines.append("  # proven on P300 hardware is 22 bytes; verify before enabling.")
        return ("text_sensor", utf_lines)

    # A field wider than 4 bytes can't go through the 1..4-byte scalar converters.
    if length > 4 and not is_bit and not enum_opts:
        return (
            "comment",
            [f"# {name} @ 0x{addr:04X}: {ev.conversion or 'raw'} length {length} > 4 bytes -> custom decode"],
        )
    # Scalar reads are clamped to the converter-supported 1..4 bytes.
    num_length = length if length in (1, 2, 3, 4) else max(1, min(4, length))

    # Converter-supported widths (mirror of the component's CONVERTERS[...]
    # .lengths -- sensor.py/number.py REJECT unsupported pairs at `esphome
    # config` time; the bulk export produced e.g. div10 at 4 bytes and
    # sec2hour at 1 byte). The effective decode width is the FIELD width,
    # which equals `length` on every scalar path (with or without block
    # extraction). Enum and bit paths never consult the converter. Fallback:
    # keep the raw bytes readable as noconv and carry the true scale as an
    # ESPHome `multiply:` filter, so the reported value stays CORRECT; the
    # signed div2/div10 default survives as an explicit `signed: true`.
    # rotatebytes is big-endian -- a scale filter cannot fix byte order, so an
    # unsupported width there stays a comment.
    if not is_bit and not enum_opts and length not in _CONV_LENGTHS.get(conv, (1, 2, 3, 4)):
        if conv == "rotatebytes":
            return (
                "comment",
                [
                    f"# {name} @ 0x{addr:04X}: RotateBytes (big-endian) at {length} bytes is not"
                    " decodable by the component (rotatebytes supports 2) -> custom decode"
                ],
            )
        if writable:
            writable = False
            note = (note + "; " if note else "") + (f"converter {conv!r} unsupported at {length} bytes -> demoted to sensor")
        scale = _CONV_SCALE.get(conv, 1.0)
        if scale != 1.0:
            value_filters.append(("multiply", scale))
        force_signed = force_signed or conv in ("div2", "div10")
        note = (note + "; " if note else "") + (
            f"converter {conv!r} unsupported at {length} bytes -> noconv"
            + (f" + multiply filter ({scale:g})" if scale != 1.0 else "")
        )
        conv = "noconv"

    poll = _poll_for(ev, conv_kind)
    lines: list[str] = []
    if note:
        lines.append(f"  # NOTE: {note}")
    if neg_opts:
        lines.append(
            f"  # NOTE: {neg_opts} negative enum option value(s) from the export omitted"
            " (raw bytes compare unsigned; a negative option can never match)"
        )

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
        state_addr = COMMAND_STATE_ADDR.get(ev.address)
        lines += [
            "- platform: vitohome",
            f"  name: {_yaml_str(name)}",
        ]
        # field_off is always 0 here: a writable field at BytePosition > 0 was
        # demoted to read-only above, because base + BytePosition is not a
        # declared write address for it.
        lines += [
            interior_addr_line,
            f"  length: {length}",
        ]
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
        state_addr = COMMAND_STATE_ADDR.get(ev.address)
        lines += [
            "- platform: vitohome",
            f"  name: {_yaml_str(name)}",
        ]
        # field_off is always 0 here (see the switch branch).
        lines += [
            interior_addr_line,
            f"  length: {length}",
        ]
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
        ]
        # field_off is always 0 here (see the switch branch).
        lines += [
            interior_addr_line,
            f"  length: {length}",
        ]
        lines.append(f"  converter: {conv}")
        # A negative lower border means the raw byte(s) are two's-complement.
        # div2/div10 are already signed in the component; every other
        # converter is unsigned by default, so it needs an explicit
        # signed: true to decode/accept a negative bound (otherwise the
        # component's number validator rejects it and the real borders would
        # fall back to the useless 0/0/1 placeholders).
        try:
            _lo_neg = lo != "" and float(lo) < 0
        except (TypeError, ValueError):
            _lo_neg = False
        _default_signed = conv in ("div2", "div10")
        is_signed = _default_signed or _lo_neg
        if _lo_neg and not _default_signed:
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
        # Vitosoft numbers bits MSB-FIRST inside the byte: index 0 is 0x80, index
        # 7 is 0x01. HARDWARE-CONFIRMED on VScotHO1_72 (0x20CB), 2026-07-09 logs:
        # 0x55DD carries exactly two datapoints, GWG_Flamme1 (BitPosition 2) and
        # GWG_Brenner_2 (BitPosition 5). The byte reads 0x01 with the burner off
        # (three samples, modulation 0 %) and 0x21 with it firing (five samples,
        # modulation 11-40 %, Kessel 25 -> 34.6 degC, Abgas 25 -> 31.3 degC).
        # LSB-first would put Flamme1 at 0x04 -- never set, i.e. "no flame" while
        # the boiler burns, and would light GWG_Brenner_2 (a second burner stage
        # this modulating unit does not have). MSB-first puts Flamme1 at 0x20,
        # which tracks the burn exactly. The previous `1 << (bit_pos % 8)` was
        # therefore mirrored for EVERY bit-field this generator emits.
        mask = 0x80 >> (bit_pos % 8)
        # Trust, but verify. `BitPosition` is normally the ABSOLUTE bit index
        # across the block, so byte = bit_pos // 8. Where the export ALSO gives a
        # non-zero BytePosition, the two must agree -- and for 5 of the 731
        # single-bit datapoints they do not:
        #   nviConsumerDmd_Attribute1_CFDM~0xA385  BitPos 24 -> byte 3, BytePos 2
        #   nvoConsumerDmd_Attribute1_LFDM~0xA346  BitPos 24 -> byte 3, BytePos 2
        #   OT ID0 LowByte Bit 10/11/12            BitPos 1/2/3, BytePos 1
        # (the OT rows are byte-relative, and are filtered out anyway as
        # OT_Physical_Read; the two nvo/nvi rows are Virtual_READ and DO reach
        # catalogs). We cannot tell which field is right, so emit a comment
        # rather than an entity that silently reads the wrong byte.
        if ev.byte_position and ev.byte_position != byte_off:
            return (
                "comment",
                [
                    f"# {name} @ 0x{addr:04X}: BitPosition {bit_pos} implies byte {byte_off} but the "
                    f"export declares BytePosition {ev.byte_position} -- contradictory, needs hardware "
                    f"confirmation before it can be emitted"
                ],
            )
        # binary_sensor reads a block at the block base and indexes byte_offset
        # inside it (aligned read -- P300 NAKs an unaligned interior read). The
        # only hard cap left is the single-telegram read limit.
        if block_len > MAX_P300_READ_LENGTH or byte_off >= block_len:
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
        # Aligned block extraction for an interior enum field: read-only, so
        # the block base is simply the read address -- no write side, no
        # state_address. The extracted width stays enum_len (derived from the
        # option values, the same bytes the interior form read at addr+off).
        enum_extract = field_off > 0 and field_off + enum_len <= block_len <= MAX_P300_READ_LENGTH
        lines += [
            "- platform: vitohome",
            "  type: enum",
            f"  name: {_yaml_str(name)}",
        ]
        if enum_extract:
            lines += [
                f"  address: 0x{addr:04X}",
                f"  length: {block_len}",
                f"  byte_offset: {field_off}",
            ]
            if enum_len != 1:
                lines.append(f"  byte_length: {enum_len}")
        else:
            lines += [
                interior_addr_line,
                f"  length: {enum_len}",
            ]
        lines += [
            "  disabled_by_default: true",
            f"  update_interval: {poll}s",
            "  options:",
        ]
        for v, label in enum_opts:
            lines.append(f"    0x{v:0{2 * enum_len}X}: {_yaml_str(label)}")
        return ("text_sensor", lines)

    # numeric sensor
    # Signedness: div2/div10 are signed by component default. For every other
    # converter (unsigned by default) an explicit `signed: true` is needed
    # when the value range crosses zero -- the access layer's LowerBorder is
    # the only signal for read-only datapoints (verified: nvoOATemp, Div100,
    # border -30..50, would otherwise decode -1.00 as +654.36). force_signed
    # carries a signed default over from a converter-width fallback.
    default_signed = conv in ("div2", "div10")
    lo_hint = ev.lower or (ev.values[0].lower if ev.values else "")
    try:
        neg_range = lo_hint != "" and float(lo_hint) < 0
    except (TypeError, ValueError):
        neg_range = False
    lines += [
        "- platform: vitohome",
        f"  name: {_yaml_str(name)}",
        addr_line,
    ]
    if use_block_extract:
        # Aligned block read + field extract (P300-portable).
        lines.append(f"  length: {block_len}")
        lines.append(f"  byte_offset: {field_off}")
        if field_width != 1:
            lines.append(f"  byte_length: {field_width}")
    else:
        lines.append(f"  length: {length}")
    lines.append(f"  converter: {conv}")
    if (force_signed or neg_range) and not default_signed:
        lines.append("  signed: true")
    unit = _unit_for(ev)
    if unit:
        lines.append(f"  unit_of_measurement: {_yaml_str(unit)}")
    dc = _device_class_for(unit)
    if dc:
        lines.append(f"  device_class: {dc}")
    if value_filters:
        # The true value transform (see the conversion resolution above); an
        # ESPHome sensor filter, applied after the raw noconv decode.
        lines.append("  filters:")
        for f_name, f_arg in value_filters:
            lines.append(f"    - {f_name}: {f_arg:.12g}")
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


# German weekday names for the per-day Schaltzeiten text entities (Monday
# first, matching the vcontrold base+day*8 convention and the component's
# decode). (label, id-suffix) pairs.
_WEEKDAYS = (
    ("Montag", "mo"),
    ("Dienstag", "di"),
    ("Mittwoch", "mi"),
    ("Donnerstag", "do"),
    ("Freitag", "fr"),
    ("Samstag", "sa"),
    ("Sonntag", "so"),
)

# The ONLY Schaltzeiten shape the component's `text` platform can decode: a
# 56-byte weekday program = 7 records of 8 bytes (four ON/OFF switch-point
# pairs per day). vito_text.cpp hardcodes SCHALTZEITEN_LEN = 8, so the other
# shapes the export carries -- 168/56 (heat-pump, 3-byte records), 168/7
# (ventilation, 24-byte records), the 24/25-byte LON vmarSchaltzeitenGroup
# blocks, and the pre-decomposed 1-byte KBUS_HV_Schaltzeit_* fields -- must
# NOT be routed here; they stay custom-decode comments.
_SCHALTZEITEN_RECORD_LEN = 8
_SCHALTZEITEN_DAYS = 7
_SCHALTZEITEN_BLOCK_LEN = _SCHALTZEITEN_RECORD_LEN * _SCHALTZEITEN_DAYS  # 56


def _is_schaltzeiten(ev: Event) -> bool:
    """A weekday switching-time program the component's `text` platform can
    decode: token names a Schaltzeiten block AND the access layer declares the
    56-byte / BlockFactor-7 / 8-byte-record shape. Other Schaltzeiten shapes
    (heat-pump, ventilation, LON group, pre-decomposed KBUS) return False and
    fall through to the generic path (custom-decode comment)."""
    if ev.address is None:
        return False
    tok = ev.token or ev.tech or ev.name or ""
    if "Schaltzeit" not in tok:
        return False
    blen = ev.byte_length or ev.block_length or 0
    bf = ev.block_factor or 0
    return blen == _SCHALTZEITEN_BLOCK_LEN and bf == _SCHALTZEITEN_DAYS


def _schaltzeiten_program_label(ev: Event) -> str:
    """Human program label from the token: 'Schaltzeiten_A1M1_HK' -> 'A1M1 HK'.
    Drops the leading 'Schaltzeiten' word, underscores -> spaces."""
    tok = (ev.token or ev.tech or "").split("~", 1)[0]
    label = re.sub(r"^Schaltzeiten_?", "", tok).replace("_", " ").strip()
    return label or tok


def _schaltzeiten_entries(ev: Event) -> list[dict]:
    """Expand one 56-byte Schaltzeiten block into 7 per-day `text` specs at
    base + day*8 (Monday first), matching the component's decode. Returns dicts
    with: address, name, seed, program (the shared program label)."""
    prog = _schaltzeiten_program_label(ev)
    seed_prog = re.sub(r"[^a-z0-9]+", "_", prog.lower()).strip("_")
    base = ev.address or 0
    entries = []
    for day, (label, sfx) in enumerate(_WEEKDAYS):
        entries.append(
            {
                "address": base + day * _SCHALTZEITEN_RECORD_LEN,
                "name": f"Schaltzeit {prog} {label}",
                "seed": f"schaltzeit_{seed_prog}_{sfx}",
                "program": prog,
            }
        )
    return entries


def _schaltzeiten_lines(entry: dict, oid: str) -> list[str]:
    """YAML lines for one per-day Schaltzeiten `text` entity. read == write at
    the same weekday address (the component re-reads what it wrote); polled
    hourly and disabled by default like every generated entity."""
    return [
        "- platform: vitohome",
        f"  name: {_yaml_str(entry['name'])}",
        f"  id: {oid}",
        f"  address: 0x{entry['address']:04X}",
        "  disabled_by_default: true",
        "  update_interval: 3600s",
    ]


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
    if re.search(r"FehlerHis", tok, re.IGNORECASE):
        return True
    return ev.address == _ERROR_HISTORY_ADDRESS


_FEHLERHIS_SLOT_RE = re.compile(r"FehlerHis\w*?(\d{1,2})\b", re.IGNORECASE)


def _error_history_slot(ev: Event) -> int | None:
    """Slot number for a FehlerHis* token (FehlerHisFA01 -> 1), else None."""
    m = _FEHLERHIS_SLOT_RE.search(ev.token or ev.tech or ev.name or "")
    return int(m.group(1)) if m else None


def _error_history_archive_tag(ev: Event) -> str:
    """A short human tag distinguishing one system archive from another on
    units that carry MORE THAN ONE (e.g. the Vitotwin gateway has both
    ecnsysEventType~Error @ 0x7507 and ecnsysEventType~VitotwinErrorHistorySW02
    @ 0x7000). The canonical archive returns "" so its entity names stay
    "Letzter Fehler" / "Fehler NN" unchanged; a genuinely distinct secondary
    archive contributes a suffix so the two do not collide (ESPHome requires
    unique names per platform).

    Only a ``prefix~subname`` token whose subname is NOT the canonical
    ``Error`` is tagged: a bare token with no ``~`` (the fixture / 0x7507
    address-fallback style) is the single canonical archive and stays
    untagged.
    """
    tok = ev.token or ev.tech or ""
    if "~" not in tok:
        return ""  # single canonical archive (fixture / address fallback)
    sub = tok.split("~", 1)[1]
    if sub == "Error":
        return ""  # the canonical ecnsysEventType~Error archive
    # "VitotwinErrorHistorySW02" -> "Vitotwin SW02": strip the ErrorHistory
    # word cluster down to something short and readable.
    tag = re.sub(r"ErrorHistory|Fehlerhistorie|History|Error|Fehler", " ", sub).strip()
    tag = re.sub(r"\s+", " ", tag)
    return tag or sub


def _error_history_entries(ev: Event) -> list[dict]:
    """Expand one error-history event into per-slot emission specs.

    Returns dicts with: address, name, seed, disabled, system (bool),
    comment (list of lines). Rules:
    - FehlerHisFA* -> ONE slot of the Feuerungsautomat (GFA burner control
      unit) history: named "GFA Fehler NN", ALL disabled by default, and
      flagged system=False so the Vitotronic fault-code map is NOT attached
      (the GFA lockout codes are a different code space).
    - ecnsysEventType~Error -> its BlockFactor slot count (the access layer
      declares BlockLength 90 / BlockFactor 10 -> ten 9-byte system slots at
      base + i*stride; vcontrold getError0..9, "Ermittle Fehlerhistory Eintrag
      1..10"); a >=90-byte block without BlockFactor keeps the legacy 10-slot
      assumption. The per-slot STRIDE is BlockLength // BlockFactor (9 for the
      standard archive, 12 for the Vitotwin's 120-byte archive); the component
      reads the leading 9 bytes (code + 8-byte BCD) at each slot address. Slot
      1 is "Letzter Fehler" (newest first -- hardware-confirmed on
      VScotHO1_72 @ 0x7507), slots 2..N disabled.
    - a SECOND system archive on the same unit gets a short tag appended to its
      names/seeds so they do not collide with the first (see
      _error_history_archive_tag).
    - any other system-history event (single slot, e.g. the 0x7507 address
      fallback) -> one enabled "Letzter Fehler" slot at its own address.
    """
    fa_slot = _error_history_slot(ev)
    if fa_slot is not None:
        # Some units carry SEVERAL FehlerHis-style archives that all match the
        # slot regex: Vitovalor has FehlerHisFA01..20 (the GFA burner archive),
        # Fehlerhistorie_FCU_0..9 and Fehlerhist_FCU_0..9 (two fuel-cell-unit
        # archives). Their slot-1 entries would all be named "GFA Fehler 01".
        # Derive a family key from the token stem (everything before the slot
        # digits) so distinct families get distinct names/seeds; the canonical
        # FehlerHisFA family stays plain "GFA Fehler NN".
        stem = re.split(r"\d", (ev.token or ev.tech or "").split("~", 1)[0], maxsplit=1)[0]
        stem = stem.rstrip("_")
        if re.fullmatch(r"FehlerHisFA", stem, re.IGNORECASE) or stem == "":
            fam_name = "GFA"
            fam_seed = "gfa"
        else:
            # The stems are already distinct across families (Fehlerhistorie_FCU
            # vs Fehlerhist_FCU vs ...), so use the stem verbatim for the name to
            # GUARANTEE uniqueness -- collapsing both to a friendly "FCU" would
            # re-introduce the collision. Underscores -> spaces for display.
            fam_name = stem.replace("_", " ").strip() or stem
            fam_seed = re.sub(r"[^a-z0-9]+", "_", stem.lower()).strip("_")
        return [
            {
                "address": ev.address,
                "name": f"{fam_name} Fehler {fa_slot:02d}",
                "seed": f"{fam_seed}_fehler_{fa_slot:02d}",
                "disabled": True,
                "system": False,
                "comment": [
                    f"# {fam_name} history slot {fa_slot} (a burner/subsystem fault archive",
                    "# DISTINCT from the Vitotronic system history). Its lockout codes are",
                    "# a different code space from the F-codes, so no codes map is attached",
                    "# and the code byte displays as raw hex. On VScotHO1_72 the GFA slot 1",
                    "# was HARDWARE-CONFIRMED distinct (read 0x00, its own timestamp 42 s",
                    "# after the system slot's 0x38). No public GFA code enumeration is",
                    "# known (export + community search both empty).",
                ],
            }
        ]
    blen = ev.byte_length or ev.block_length or 9
    # The slot structure is explicit in the access layer where present:
    # BlockLength = BlockFactor records (ecnsysEventType~Error: 90 bytes,
    # BlockFactor 10, MappingType 3 -> ten slots). The per-slot stride is the
    # record size BlockLength // BlockFactor; the component's error_history
    # type reads the leading 9 bytes (code + 8-byte BCD timestamp) at each slot
    # address, so a 12-byte-record archive (Vitotwin, 120/10) still advances by
    # 12 even though only 9 are decoded. The legacy >=90 heuristic (stride 9)
    # stays for exports/fixtures that carry no BlockFactor.
    bf = ev.block_factor or 0
    if bf > 1 and blen % bf == 0 and blen // bf >= 9:
        slots = bf
        stride = blen // bf
    elif blen >= 90:
        slots = 10
        stride = 9
    else:
        slots = 1
        stride = 9
    tag = _error_history_archive_tag(ev)
    name_suffix = f" ({tag})" if tag else ""
    seed_suffix = "_" + re.sub(r"[^a-z0-9]+", "_", tag.lower()).strip("_") if tag else ""
    if slots > 1:
        entries = []
        for i in range(slots):
            slot = i + 1
            entries.append(
                {
                    "address": (ev.address or 0) + i * stride,
                    "name": ("Letzter Fehler" if slot == 1 else f"Fehler {slot:02d}") + name_suffix,
                    "seed": ("letzter_fehler" if slot == 1 else f"fehler_{slot:02d}") + seed_suffix,
                    "disabled": slot > 1,
                    "system": True,
                    "comment": [
                        f"# System fault history slot {slot} of {slots} ({ev.token or 'ecnsysEventType~Error'},",
                        f"# {blen}-byte block at 0x{ev.address:04X} + {i}*{stride}; vcontrold getError{i}).",
                    ]
                    + (["# Slot 1 = newest entry -- hardware-confirmed on VScotHO1_72."] if slot == 1 else []),
                }
            )
        return entries
    return [
        {
            "address": ev.address,
            "name": "Letzter Fehler" + name_suffix,
            "seed": "letzter_fehler" + seed_suffix,
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
        re.search(r"Betriebsart|BedienteilBA|BedienBetriebsart", ev.token or ev.tech or "", re.IGNORECASE)
    )


# Fault / status / health registers. Tagged entity_category: diagnostic so they
# group under Diagnostics in Home Assistant rather than mixing with measurements.
# Covers: fault bytes (GFA 0x5738, EEPROM/I2C), the LON alarm record (0xA132),
# sensor-status enums (TemperaturFehler_*, *Sensor*Status*, 0x089C.., 0xCF90),
# collective-fault flags (Sammel*), and the system/maintenance status enums.
_DIAGNOSTIC_RE = re.compile(
    r"Fehler|Stoer|St[oö]r|Status|Sensor.*Status|SensorStatus|Alarm|EEPROM|I2C|"
    r"Diagnos|Sammel|Wartung|Quitt|TemperaturFehler|ecnStatusEventType",
    re.IGNORECASE,
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
        return conv_kind in (DIV, COUNTER)
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
    order: str = "address",
    stats: dict | None = None,
) -> str:
    """Render the package for one device token. ``order`` is "address"
    (default; entities sorted by Optolink address, byte-identical to earlier
    revisions) or "group" (entities sorted by the Vitosoft navigation tree --
    ecnEventTypeGroup -- with a section comment per functional group).
    ``stats``, if a dict is passed, receives {"entities": <emitted count>,
    "comments": <custom-decode hint count>} so callers (export_all) can tell a
    real catalog from an empty shell without parsing the output."""
    events = catalog.events_for(device)
    if not events:
        raise SystemExit(
            f"device {device!r} not found or has no events. Run with --list-devices to see available device tokens."
        )

    # Resolve the fault-code map once: the chosen set, or None when codes are off.
    codes_map = fault_codes.SETS.get(error_code_set) if error_codes else None
    codes_set_name = error_code_set if error_codes else ""

    inc = re.compile(include_re) if include_re else None
    exc = re.compile(exclude_re) if exclude_re else None

    # Whether this unit has any ordinary Optolink datapoint that actually
    # emits a real entity. A unit whose every real datapoint needs KBUS/GFA/RPC
    # access (Dekamatik cascades, M-Bus meters, ...) -- OR whose only reachable
    # rows are things emit_entity turns into comments (a bare status enum with
    # no options, a >4-byte blob) -- is not an Optolink device, and emitting
    # the generic 0x7507 system fault history for it would poll an address that
    # can never answer (the earlier bulk export produced 172 such history-only
    # files). This is decided AFTER the main emission pass on the true count of
    # emitted entities, not a structural pre-check: the status enum at 0x7561
    # passes every cheap "reachable datapoint" predicate yet emits nothing.
    # System-history events are collected here and emitted in a second pass;
    # FehlerHis* per-slot archives carry their own FCRead and are gated only on
    # that, since they are genuine per-unit datapoints.
    deferred_history: list[Event] = []

    # Per-entity chunks: (sort_key, group_label, lines). Emitted per platform
    # in key order; in "group" order a section comment is inserted whenever
    # the group label changes. In "address" order keys are the processing
    # sequence (events are pre-sorted by address), so output is unchanged.
    buckets: dict[str, list[tuple[tuple, str, list[str]]]] = {
        "sensor": [],
        "binary_sensor": [],
        "number": [],
        "select": [],
        "switch": [],
        "text": [],
        "text_sensor": [],
    }
    comments: list[str] = []
    seq = 0

    def _add_chunk(platform: str, ev: Event | None, lines: list[str]):
        """Append one entity's lines with its sort key and group label."""
        nonlocal seq
        seq += 1
        if order == "group" and ev is not None:
            gsort, glabel = catalog.group_for(ev, device)
        else:
            gsort, glabel = (), ""  # () sorts first: hub-fed entities lead
        buckets[platform].append(((gsort, seq), glabel, ["  " + ln if ln else "" for ln in lines]))

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
    real_entities = 0  # emitted entities that are NOT the hub-fed device_id
    dropped_unreachable = 0
    history_suppressed = False

    # Hub-fed device identity (covers the 0xF8..0xFB identification reads, which
    # are then suppressed below).
    if emit_device_id:
        oid = _make_obj_id("device_type", used_ids)
        _add_chunk("text_sensor", None, _device_id_lines(oid))
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
            # code-map-free. The system block expands into its BlockFactor
            # slots. Reachability: a FehlerHis* slot is checked on its own
            # FCRead like any datapoint; the generic system archive (a row
            # shared by every unit) additionally requires the device to emit
            # at least one real entity at all -- decided in the second pass
            # below (KBUS-only Dekamatik units, M-Bus meters -> 0x7507 can
            # never answer, so the catalog would be a phantom history).
            if reachable_only and not _is_reachable(ev):
                dropped_unreachable += 1
                continue
            deferred_history.append(ev)
            continue

        # Weekday switching-time programs: one 56-byte token -> 7 per-day `text`
        # entities at base + day*8 (only the 8-byte-record shape; see
        # _is_schaltzeiten). Handled here because one datapoint fans out to
        # seven entities, like error history.
        if _is_schaltzeiten(ev):
            if reachable_only and not _is_reachable(ev):
                dropped_unreachable += 1
                continue
            if ev.address is not None and _field_key(ev) in seen_addr:
                continue
            if ev.address is not None:
                seen_addr.add(_field_key(ev))
            for entry in _schaltzeiten_entries(ev):
                oid = _make_obj_id(entry["seed"], used_ids)
                _add_chunk("text", ev, _schaltzeiten_lines(entry, oid))
                kept += 1
                real_entities += 1
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
        _add_chunk(platform, ev, lines)
        kept += 1
        real_entities += 1

    # Second pass: system fault history. Now that the main pass is done, the
    # true count of emitted real entities is known -- gate the generic system
    # archive on it (a device that emitted nothing real is not on Optolink).
    # FehlerHis* archives are genuine per-unit datapoints and always emit.
    for ev in deferred_history:
        if real_entities == 0 and _error_history_slot(ev) is None:
            history_suppressed = True
            continue
        for entry in _error_history_entries(ev):
            if entry["address"] is None:
                continue
            key = (entry["address"], 0, 0)
            if key in seen_addr:
                continue
            seen_addr.add(key)
            oid = _make_obj_id(entry["seed"], used_ids)
            _add_chunk("text_sensor", ev, _error_history_lines(entry, oid, codes_map, codes_set_name))
            kept += 1
            real_entities += 1

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
    if history_suppressed:
        out.append("# System error history omitted: this unit has NO Optolink-reachable")
        out.append("# datapoints (KBUS/GFA/RPC-only device), so the generic 0x7507 fault")
        out.append("# archive can never answer. Re-run with --no-reachable-only to force it.")
        out.append("#")
    out.append("# VERIFY ON HARDWARE: not every address answers on every firmware.")
    out.append("# Run `esphome config` then `esphome compile`/`run` before relying")
    out.append("# on any value. number entities whose borders were absent in the")
    out.append("# export get min/max/step = 0/0/1 placeholders: valid config, but")
    out.append("# pinned to 0 until you fill in the real bounds.")
    out.append("# ============================================================")
    out.append("")

    for platform in ("sensor", "binary_sensor", "number", "select", "switch", "text", "text_sensor"):
        if buckets[platform]:
            out.append(f"{platform}:")
            last_label = None
            for _key, label, chunk in sorted(buckets[platform], key=lambda c: c[0]):
                if order == "group" and label and label != last_label:
                    out.append(f"  # --- {label} ---")
                last_label = label
                out.extend(chunk)
            out.append("")

    if comments:
        out.append("# --- datapoints needing custom decode (not auto-emitted) ---")
        out.extend(comments)
        out.append("")

    if stats is not None:
        stats["entities"] = kept
        stats["real_entities"] = real_entities  # emitted entities excluding the hub-fed device_id
        stats["comments"] = len(comments)

    return "\n".join(out)


# --- bulk export -----------------------------------------------------------
# One catalog file per device token. The Vitosoft token IDs already follow
# Viessmann's ``unit_swIndex[_variant]`` naming -- e.g. ``VScotHO1_72`` is unit
# ``VScotHO1`` at software index 72 (its IdentificationExtension low byte 0x48 =
# 72), and hardware variants are *distinct* unit names (``GWG_VBES`` vs
# ``GWG_VWMS`` vs ``GWG_VBT2``), not a numeric suffix. So the token itself is the
# ``unit[_swIndex][_variant]`` key: the file name is just the lower-cased token.

_FILENAME_SAFE_RE = re.compile(r"[^a-z0-9._-]+")


def _export_stem(token: str) -> str:
    """Filesystem-safe, lower-cased file stem for a device token.

    The token already encodes ``unit`` and (where Viessmann assigns one) the
    software index, so no parsing is needed: lower-case it and replace any
    character outside ``[a-z0-9._-]`` so the name is portable. ``VScotHO1_72``
    -> ``vscotho1_72``.
    """
    stem = _FILENAME_SAFE_RE.sub("_", token.strip().lower())
    return stem.strip("._-") or "device"


def _identity_fields(row: dict) -> dict:
    """Identification indices for one ``ecnDataPointType`` row, for the manifest.

    ``ident`` is the device Identification (0xF8F9). The hardware index and the
    software-index range are reported ONLY when the extension is the ordinary
    2-byte ``HW<<8 | SW`` boiler/controller form (matches ``Catalog.resolve()``):
    the high byte is the categorical hardware generation, the low byte the
    software index. A handful of device types -- M-Bus meters, some telecom
    modules -- carry a longer serial-number extension instead; for those the
    numeric columns stay blank and the raw extension string is preserved so the
    row is still traceable. Everything here is read straight from the export --
    nothing is inferred.
    """
    ext = row.get("ext")
    extt = row.get("extt")
    ident = row.get("ident")
    out = {
        "ident": f"0x{ident:04X}" if ident is not None else "",
        "hw_index": "",
        "sw_lo": "",
        "sw_hi": "",
        "f0_lo": "",
        "f0_hi": "",
        "ext_raw": row.get("IdentificationExtension") or "",
    }
    if ext is not None and 0 <= ext <= 0xFFFF:
        out["hw_index"] = ext >> 8
        out["sw_lo"] = ext & 0xFF
        out["sw_hi"] = (extt & 0xFF) if (extt is not None and 0 <= extt <= 0xFFFF) else ext & 0xFF
    f0 = row.get("f0")
    if f0 is not None:
        f0t = row.get("f0t")
        out["f0_lo"] = f0
        out["f0_hi"] = f0t if f0t is not None else f0
    return out


_MANIFEST_COLUMNS = (
    "file",
    "token",
    "ident",
    "hw_index",
    "sw_lo",
    "sw_hi",
    "f0_lo",
    "f0_hi",
    "events",  # datapoints LINKED to the unit in the export
    "entities",  # entities actually EMITTED into the file (post filters/profile)
    "bytes",
    "status",
    "ext_raw",
)


def export_all(
    catalog: Catalog,
    out_dir: str,
    *,
    profile: str,
    include_re: str | None,
    exclude_re: str | None,
    token_filter: str | None,
    suffix: str,
    emit_device_id: bool,
    emit_error_history: bool,
    error_codes: bool,
    error_code_set: str,
    reachable_only: bool,
    order: str = "address",
) -> int:
    """Write one catalog per device token into *out_dir* (created if needed).

    Files are named ``<token><suffix>`` (lower-cased, sanitised) -- Viessmann's
    ``unit_swIndex[_variant]`` key -- and an ``index.csv`` manifest is written
    alongside mapping every file back to its identification signature (ident /
    hardware index / software-index range / F0 range / event count / status).

    Each device is generated with the SAME options the single-device path uses,
    so ``--export-all`` is "run the generator for every unit with these flags".
    One unit that fails to generate is recorded and skipped; it does not abort
    the batch. Returns a process exit code (0 if at least one file was written,
    1 otherwise).
    """
    if os.path.exists(out_dir) and not os.path.isdir(out_dir):
        print(f"--out {out_dir!r} exists and is not a directory", file=sys.stderr)
        return 1
    os.makedirs(out_dir, exist_ok=True)

    pat = re.compile(token_filter) if token_filter else None
    ident_by_token = {r["token"]: r for r in catalog._ident if r.get("token")}

    tokens = sorted(catalog.devices)
    if pat:
        tokens = [t for t in tokens if pat.search(t)]
    if not tokens:
        where = f" matching {token_filter!r}" if token_filter else ""
        print(f"no device tokens{where} to export", file=sys.stderr)
        return 1

    if error_codes:
        print(
            f"note: --export-all attaches the same fault-code map ({error_code_set!r}) to "
            "every unit, but fault-code semantics are device-variant-specific -- pass "
            "--no-error-codes for a neutral bulk export, or regenerate an individual unit "
            "with the correct --error-code-set. See index.csv for what was written.",
            file=sys.stderr,
        )

    manifest: list[dict] = []
    used: dict[str, str] = {}  # stem -> token, to catch sanitiser collisions
    written = skipped = failed = 0

    for token in tokens:
        row = ident_by_token.get(token, {})
        n_events = len(catalog.events_for(token))
        rec = {
            "file": "",
            "token": token,
            **_identity_fields(row),
            "events": n_events,
            "entities": "",
            "bytes": "",
            "status": "",
        }

        if n_events == 0:
            rec["status"] = "skipped: no events"
            manifest.append(rec)
            skipped += 1
            continue

        stem = _export_stem(token)
        if used.get(stem, token) != token:  # distinct token collided after sanitising
            n = 2
            while f"{stem}-{n}" in used:
                n += 1
            stem = f"{stem}-{n}"
        used[stem] = token
        fname = stem + suffix

        try:
            gen_stats: dict = {}
            text = generate(
                catalog,
                token,
                profile,
                include_re,
                exclude_re,
                emit_device_id=emit_device_id,
                emit_error_history=emit_error_history,
                error_codes=error_codes,
                error_code_set=error_code_set,
                reachable_only=reachable_only,
                order=order,
                stats=gen_stats,
            )
        except SystemExit as exc:  # generate() uses this for "no emittable events"
            rec["status"] = f"skipped: {exc}"
            manifest.append(rec)
            skipped += 1
            continue
        except Exception as exc:  # noqa: BLE001 -- one bad unit must not abort the batch
            rec["status"] = f"error: {type(exc).__name__}: {exc}"
            manifest.append(rec)
            failed += 1
            continue

        rec["entities"] = gen_stats.get("entities", "")
        # A device with NO real entity (only the hub-fed device_id, plus at
        # most some "needs custom decode" / unreachable comments) is not an
        # Optolink unit: every genuine datapoint of a KBUS-only Dekamatik
        # cascade or M-Bus meter needs an access method VitoWiFi can't drive.
        # Writing such a file is meaningless -- the earlier bulk export
        # produced 220 of them (48 header-only + 172 that were nothing but a
        # phantom 0x7507 fault history). Comments alone never make a usable
        # catalog, so the skip keys on the real-entity count, not the raw
        # `entities` (which counts the device_id) or the comment count.
        if gen_stats.get("real_entities", 0) == 0:
            rec["status"] = "skipped: no Optolink-reachable datapoints"
            manifest.append(rec)
            skipped += 1
            continue

        data = text.encode("utf-8")
        with open(os.path.join(out_dir, fname), "wb") as fh:
            fh.write(data)
        rec["file"] = fname
        rec["bytes"] = len(data)
        rec["status"] = "ok"
        manifest.append(rec)
        written += 1

    manifest.sort(key=lambda r: r["token"])
    index_path = os.path.join(out_dir, "index.csv")
    with open(index_path, "w", encoding="utf-8", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=_MANIFEST_COLUMNS)
        w.writeheader()
        for rec in manifest:
            w.writerow({k: rec.get(k, "") for k in _MANIFEST_COLUMNS})

    print(
        f"wrote {written} catalog(s) to {out_dir}  (skipped {skipped}, failed {failed}); manifest: {index_path}",
        file=sys.stderr,
    )
    if failed:
        print(f"warning: {failed} unit(s) failed to generate -- see 'error:' rows in index.csv", file=sys.stderr)
    return 0 if written else 1


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
    p.add_argument(
        "--culture",
        default="de",
        help="Textresource language for names/labels (de,en,fr,it,ru,nl,pl,da,hu,es,tr,lt,cs); "
        "note the 2026 export ships no translated entity strings, so this is currently a no-op",
    )
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
    p.add_argument(
        "--export-all",
        action="store_true",
        help="generate one catalog per device token into --out (a directory); file names follow "
        "Viessmann's unit_swIndex[_variant] token (e.g. vscotho1_72.yaml) plus a fault-code note, "
        "and an index.csv manifest maps each file to its ident/HW/SW signature",
    )
    p.add_argument(
        "--export-filter",
        help="with --export-all: regex; only export tokens whose ID matches "
        "(e.g. '^V' for Vitotronic controllers, '^GWG_' for GWG units)",
    )
    p.add_argument(
        "--export-suffix",
        default=".yaml",
        help="with --export-all: file-name extension for each catalog (default: .yaml)",
    )
    p.add_argument(
        "--order",
        choices=("address", "group"),
        default="address",
        help="entity ordering: 'address' (default; sorted by Optolink address) or "
        "'group' (grouped by the Vitosoft navigation tree with a section comment per group)",
    )
    p.add_argument("--out", help="output file (default: stdout); with --export-all, the output DIRECTORY")
    args = p.parse_args(argv)

    catalog = load_catalog(args.data, culture=args.culture)

    if args.export_all:
        if not args.out:
            p.error("--export-all requires --out <directory>")
        return export_all(
            catalog,
            args.out,
            profile=args.profile,
            include_re=args.include,
            exclude_re=args.exclude,
            token_filter=args.export_filter,
            suffix=args.export_suffix,
            emit_device_id=args.device_id,
            emit_error_history=args.error_history,
            error_codes=args.error_codes,
            error_code_set=args.error_code_set,
            reachable_only=args.reachable_only,
            order=args.order,
        )

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
        order=args.order,
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
