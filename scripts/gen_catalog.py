#!/usr/bin/env python3
"""Generate a vitohome ESPHome package from a Viessmann Vitosoft XML export.

The per-device datapoint definitions (address, conversion, access type, value
enums, units, borders) are NOT discoverable over the Optolink bus -- they come
from Viessmann's own Vitosoft data, surfaced by the InsideViessmannVitosoft
scripts as a ``DPDefinitions.xml`` .NET DataSet diffgram. Hand-picking
datapoints out of that XML is tedious and error-prone, so this script does it:
pick a device, pick a profile, get a ready-to-include ESPHome package with each
datapoint on the right platform (sensor / binary_sensor / number / select /
text_sensor), the correct ``converter:``, and a sensible poll interval.

Typical use::

    # which device tables does this export contain?
    python3 scripts/gen_catalog.py --data /path/to/vitosoft-export --list-devices

    # generate a standard package for the reference unit and slow-poll counters
    python3 scripts/gen_catalog.py \
        --data /path/to/vitosoft-export \
        --device VScotHO1_72 \
        --profile standard \
        --out my-heater.vitohome.yaml

The emitted file is an ESPHome *package*: include it from your device YAML with

    packages:
      heater: !include my-heater.vitohome.yaml

and define the ``vitohome:`` hub (and ``uart:``) yourself. Every entity is
``disabled_by_default: true`` so you opt in from Home Assistant; flip the ones
you care about. Always run ``esphome config`` and ``esphome compile`` before a
live run -- this generator gets you a correct starting point, not a guarantee
that every address answers on YOUR firmware.

This script intentionally has no third-party dependencies (stdlib only); it can
run in CI and in a bare Python install.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field

# --- conversion -> vitohome converter --------------------------------------
# Maps the Vitosoft ``Conversion`` name to a vitohome ``converter:`` plus a
# "kind" used for platform/poll decisions. None converter means "numeric but no
# scaling preset" -> emit as noconv (raw). Conversions we cannot represent as a
# numeric sensor are marked SKIP with a reason (emitted as a commented hint).
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
    "RotateBytes": "byte array -> custom decode",
}

# Poll tiers (seconds).
POLL_FAST = 60  # live temperatures / measurements
POLL_SLOW = 600  # monotonic counters (hours, starts, consumption)
POLL_CODING = 3600  # writable coding values / setpoints (rarely change)
POLL_ERROR = 300  # error history

PROFILES = ("minimal", "standard", "full")


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
    name: str  # "Name~0xADDR" or just a name
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
    values: list = field(default_factory=list)


def _txt(elem, tag):
    child = elem.find(tag)
    if child is None or child.text is None:
        return None
    return child.text.strip()


def _int(elem, tag):
    v = _txt(elem, tag)
    if v is None or v == "":
        return None
    try:
        return int(v, 0)
    except ValueError:
        return None


def _address_from_name(name: str) -> int | None:
    # Event names look like "Outside_Temp~0x0800"; the Optolink address is the
    # hex after the '~'.
    if name and "~" in name:
        tail = name.rsplit("~", 1)[1]
        try:
            return int(tail, 16)
        except ValueError:
            return None
    return None


def _strip_local(tag: str) -> str:
    # ElementTree namespaces tags as '{uri}local'; we match on the local name.
    return tag.rsplit("}", 1)[-1]


def _iter(elem, local_name: str):
    for child in elem.iter():
        if _strip_local(child.tag) == local_name:
            yield child


class Catalog:
    """Parsed Vitosoft DataSet: datapoint types + events + value types."""

    def __init__(self, root: ET.Element):
        self.root = root
        # name/address token -> datapoint type Id
        self.devices: dict[str, str] = {}
        self._dp_name: dict[str, str] = {}  # id -> display name
        self._dp_by_id: dict[str, str] = {}  # id -> address token
        self._links: dict[str, list] = {}  # dp_type_id -> [event_id]
        self._events: dict[str, Event] = {}
        self._evt_value_links: dict[str, list] = {}  # event_id -> [value_id]
        self._values: dict[str, EventValue] = {}
        self._parse()

    def _parse(self):
        for dp in _iter(self.root, "ecnDatapointType"):
            dp_id = _txt(dp, self._ns(dp, "Id")) or _txt(dp, "Id")
            name = _txt(dp, self._ns(dp, "Name")) or _txt(dp, "Name")
            address = _txt(dp, self._ns(dp, "Address")) or _txt(dp, "Address")
            if dp_id is None:
                continue
            token = address or name or dp_id
            self._dp_name[dp_id] = name or token
            self._dp_by_id[dp_id] = token
            self.devices[token] = dp_id

        for link in _iter(self.root, "ecnDataPointTypeEventTypeLink"):
            dp_id = _txt(link, "DataPointTypeId")
            ev_id = _txt(link, "EventTypeId")
            if dp_id and ev_id:
                self._links.setdefault(dp_id, []).append(ev_id)

        for ev in _iter(self.root, "ecnEventType"):
            ev_id = _txt(ev, "Id")
            if ev_id is None:
                continue
            name = _txt(ev, "Name") or ""
            address = _int(ev, "Address")
            if address is None:
                address = _address_from_name(name)
            self._events[ev_id] = Event(
                id=ev_id,
                name=name,
                address=address,
                conversion=_txt(ev, "Conversion") or "",
                access_type=_int(ev, "Type") or 1,
                block_length=_int(ev, "BlockLength"),
                byte_length=_int(ev, "ByteLength"),
                byte_position=_int(ev, "BytePosition"),
                bit_length=_int(ev, "BitLength"),
                bit_position=_int(ev, "BitPosition"),
                lower=_txt(ev, "LowerBorder") or "",
                upper=_txt(ev, "UpperBorder") or "",
                stepping=_txt(ev, "Stepping") or "",
                enum_type=(_txt(ev, "EnumType") or "").lower() in ("1", "true"),
            )

        for link in _iter(self.root, "ecnEventTypeEventValueTypeLink"):
            ev_id = _txt(link, "EventTypeId")
            val_id = _txt(link, "EventValueTypeId")
            if ev_id and val_id:
                self._evt_value_links.setdefault(ev_id, []).append(val_id)

        for val in _iter(self.root, "ecnEventValueType"):
            val_id = _txt(val, "Id")
            if val_id is None:
                continue
            self._values[val_id] = EventValue(
                name=_txt(val, "Name") or "",
                enum_address_value=_int(val, "EnumAddressValue"),
                enum_replace_value=_txt(val, "EnumReplaceValue") or "",
                unit=_txt(val, "Unit") or "",
                lower=_txt(val, "LowerBorder") or "",
                upper=_txt(val, "UpperBorder") or "",
                stepping=_txt(val, "Stepping") or "",
            )

    @staticmethod
    def _ns(elem, local):
        # Return the tag string actually present (namespaced or not) for a local
        # name, so find() works regardless of namespace.
        for child in elem:
            if _strip_local(child.tag) == local:
                return child.tag
        return local

    def events_for(self, device_token: str) -> list:
        dp_id = self.devices.get(device_token)
        if dp_id is None:
            return []
        out = []
        for ev_id in self._links.get(dp_id, []):
            ev = self._events.get(ev_id)
            if ev is None:
                continue
            # attach value types (enums / unit / borders)
            for val_id in self._evt_value_links.get(ev_id, []):
                val = self._values.get(val_id)
                if val is not None:
                    ev.values.append(val)
            out.append(ev)
        return out


def load_catalog(data_dir: str) -> Catalog:
    """Load the first DPDefinitions*.xml found under *data_dir*."""
    candidates = []
    for dirpath, _dirs, files in os.walk(data_dir):
        for f in files:
            if re.match(r"DPDefinitions.*\.xml$", f, re.IGNORECASE):
                candidates.append(os.path.join(dirpath, f))
    if not candidates:
        raise SystemExit(
            f"no DPDefinitions*.xml found under {data_dir!r}. "
            "Point --data at the directory containing the Vitosoft XML export."
        )
    path = sorted(candidates)[0]
    tree = ET.parse(path)
    return Catalog(tree.getroot())


# --- entity emission -------------------------------------------------------


def _friendly(name: str) -> str:
    """Turn an event id into a human-ish entity name.

    The 2026 Vitosoft export does not ship display names, so we fall back to a
    cleaned-up technical id: drop the ``~0xADDR`` suffix, split snake/camel,
    title-case.
    """
    base = name.split("~", 1)[0]
    base = base.replace("_", " ").strip()
    base = re.sub(r"(?<=[a-z])(?=[A-Z])", " ", base)
    base = re.sub(r"\s+", " ", base)
    return base[:1].upper() + base[1:] if base else name


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
    return ""


def _yaml_str(s: str) -> str:
    # Always double-quote and escape, so umlauts / specials never break parsing.
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def emit_entity(ev: Event, profile: str) -> tuple[str, list[str]] | None:
    """Return (platform, yaml_lines) for one event, or None to skip it."""
    name = _friendly(ev.name)
    addr = ev.address
    if addr is None:
        return None

    # Non-numeric conversions: surface as a commented hint, never a wrong sensor.
    if ev.conversion in NON_NUMERIC_CONVERSIONS:
        hint = NON_NUMERIC_CONVERSIONS[ev.conversion]
        return (
            "comment",
            [f"# {name} @ 0x{addr:04X}: {ev.conversion} ({hint})"],
        )

    conv, conv_kind = CONVERSION_MAP.get(ev.conversion, (None, PLAIN))
    note = None
    if conv is None:
        conv = "noconv"
        note = f"conversion {ev.conversion!r} has no preset; raw noconv emitted"

    length = ev.block_length or ev.byte_length or 1
    if length not in (1, 2, 3, 4):
        length = max(1, min(4, length))

    enum_opts = _enum_options(ev)
    is_bit = (ev.bit_length or 0) > 0
    writable = ev.access_type in (2, 3)
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
        # Borders/stepping are best-effort hints; if absent, the user must set
        # min/max/step (number requires them). Emit what we have, comment the
        # rest so `esphome config` tells the user exactly what to fill in.
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
        lines += [
            "- platform: vitohome",
            f"  name: {_yaml_str(name)}",
            f"  address: 0x{addr:04X}",
            f"  length: {length}",
        ]
        if ev.byte_position:
            lines.append(f"  byte_offset: {ev.byte_position}")
        if ev.bit_position is not None:
            lines.append(f"  bit_mask: 0x{(1 << ev.bit_position) & 0xFF:02X}")
        lines.append("  disabled_by_default: true")
        lines.append(f"  update_interval: {poll}s")
        return ("binary_sensor", lines)

    if enum_opts:
        lines += [
            "- platform: vitohome",
            "  type: enum",
            f"  name: {_yaml_str(name)}",
            f"  address: 0x{addr:04X}",
            f"  length: {length}",
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


def _profile_keep(ev: Event, profile: str) -> bool:
    if profile == "full":
        return True
    conv, conv_kind = CONVERSION_MAP.get(ev.conversion, (None, PLAIN))
    writable = ev.access_type in (2, 3)
    if profile == "standard":
        # everything except non-numeric blobs
        return ev.conversion not in NON_NUMERIC_CONVERSIONS
    # minimal: live measurements (DIV temps), key counters, and writable values
    if profile == "minimal":
        if writable:
            return True
        if conv_kind in (DIV, COUNTER):
            return True
        return False
    return True


def generate(catalog: Catalog, device: str, profile: str, include_re: str | None, exclude_re: str | None) -> str:
    events = catalog.events_for(device)
    if not events:
        raise SystemExit(
            f"device {device!r} not found or has no events. " "Run with --list-devices to see available device tokens."
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
    kept = 0

    for ev in sorted(events, key=lambda e: (e.address or 0)):
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
        # indent entity lines by two spaces under the platform key
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
    p.add_argument(
        "--data",
        required=True,
        help="directory containing the Vitosoft XML export (DPDefinitions*.xml)",
    )
    p.add_argument("--device", help="device token (Address) to generate for, e.g. VScotHO1_72")
    p.add_argument("--list-devices", action="store_true", help="list device tokens in the export and exit")
    p.add_argument(
        "--profile",
        choices=PROFILES,
        default="standard",
        help="how many datapoints to emit (default: standard)",
    )
    p.add_argument("--include", help="regex; only emit events whose name matches")
    p.add_argument("--exclude", help="regex; drop events whose name matches")
    p.add_argument("--out", help="output file (default: stdout)")
    args = p.parse_args(argv)

    catalog = load_catalog(args.data)

    if args.list_devices:
        for token in sorted(catalog.devices):
            dp_id = catalog.devices[token]
            display = catalog._dp_name.get(dp_id, "")
            n = len(catalog.events_for(token))
            line = f"{token}\t{display}\t({n} events)"
            print(line)
        return 0

    if not args.device:
        p.error("--device is required (or use --list-devices)")

    text = generate(catalog, args.device, args.profile, args.include, args.exclude)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(text)
        print(f"wrote {args.out}", file=sys.stderr)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
