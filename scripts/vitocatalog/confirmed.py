"""Hardware findings that correct or complete the Vitosoft export.

The export is incomplete and sometimes wrong for a given unit: a datapoint the
controller carries is missing from its device table, or a row declares a byte
order the firmware does not use. Findings confirmed on a real unit live in
``confirmed.toml`` next to this module and are applied in
``Catalog.events_for``, so every later stage (writability, converters, the
renderer) treats a corrected datapoint like any other.

A finding applies to the device tokens it names and nowhere else. Sibling
revisions often share a row, but a fact seen on one revision is not evidence
for another: a token without its own confirmation needs a reason in the TOML.

Each ``[[finding]]`` table:

    tech      technical id of the datapoint (the catalog's ``id:``)
    action    "confirm" (mark one the device has as checked on hardware),
              "add" (link a Vitosoft datapoint of another device to it),
              "define" (one the export has on no device),
              "change" (override fields of one the device has) or
              "drop" (remove it from the device)
    devices   exact device tokens, e.g. ["GWG_VBES_03"]
    checked   "read" (values read and matched the control panel or a gauge)
              or "write" (written and read back)
    what      one line: what the unit showed
    unit      how the unit identified itself, e.g. "0x2053 HW=0x01 SW=0x1E"
    source    where the evidence is, e.g. "SoulSolistice/esphome_vitohome#35"
    set       (add, change, define) Event fields, see SETTABLE; a define
              needs DEFINE_REQUIRED
    name      (define) display name per catalog language, { de = .., en = .. }
    from      (add) device token to take the row from when the export carries
              the tech id in differing definitions
    group_of  (add, define) tech id of a datapoint on the device whose menu
              group the new one joins; without it the entity is ungrouped

stdlib only (tomllib), like the rest of the generator.
"""

from __future__ import annotations

import dataclasses
from dataclasses import dataclass, field
import os
import tomllib

from .models import Event

DEFAULT_PATH = os.path.join(os.path.dirname(__file__), "confirmed.toml")

ACTIONS = ("confirm", "add", "define", "change", "drop")
CHECKS = ("read", "write")
_REQUIRED = ("tech", "action", "devices", "checked", "what", "unit", "source")
_OPTIONAL = ("set", "name", "from", "group_of")
NAME_CULTURES = ("de", "en")

# Event fields a finding may override, with the accepted TOML types. Identity
# fields (id, tech, token) and the enum value list are not settable.
SETTABLE: dict[str, tuple[type, ...]] = {
    "address": (int,),
    "conversion": (str,),
    "access_type": (int,),
    "block_length": (int,),
    "byte_length": (int,),
    "byte_position": (int,),
    "bit_length": (int,),
    "bit_position": (int,),
    "lower": (str,),
    "upper": (str,),
    "stepping": (str,),
    "unit": (str,),
    "fc_read": (str,),
    "fc_write": (str,),
    "parameter_type": (str,),
    "conv_factor": (int, float),
    "conv_offset": (int, float),
    "big_endian": (bool,),
}

# What a define must state: where the value is and how it is read.
DEFINE_REQUIRED = ("address", "conversion", "access_type", "block_length", "byte_length", "byte_position", "fc_read")


class FindingError(ValueError):
    """A malformed finding, or one that does not fit the export."""


@dataclass(frozen=True)
class Finding:
    tech: str
    action: str
    devices: tuple[str, ...]
    checked: str
    what: str
    unit: str
    source: str
    set: dict = field(default_factory=dict)
    name: dict = field(default_factory=dict)
    from_device: str = ""
    group_of: str = ""

    @property
    def label(self) -> str:
        """The finding as one line, rendered above the entity."""
        origin = " (not in the Vitosoft export)" if self.action == "define" else ""
        return f"{self.what}{origin} [{self.checked} on {self.unit}; {self.source}]"


def _parse(entry: dict, where: str) -> Finding:
    if not isinstance(entry, dict):
        raise FindingError(f"{where}: not a table")
    unknown = sorted(set(entry) - set(_REQUIRED) - set(_OPTIONAL))
    if unknown:
        raise FindingError(f"{where}: unknown key(s) {', '.join(unknown)}")
    missing = [k for k in _REQUIRED if k not in entry]
    if missing:
        raise FindingError(f"{where}: missing {', '.join(missing)}")
    for key in ("tech", "action", "checked", "what", "unit", "source", "from", "group_of"):
        if key in entry and not (isinstance(entry[key], str) and entry[key].strip()):
            raise FindingError(f"{where}: {key} must be a non-empty string")
    where = f"{where} ({entry['tech']})"
    if entry["action"] not in ACTIONS:
        raise FindingError(f"{where}: action must be one of {', '.join(ACTIONS)}")
    if entry["checked"] not in CHECKS:
        raise FindingError(f"{where}: checked must be one of {', '.join(CHECKS)}")
    devices = entry["devices"]
    if not (isinstance(devices, list) and devices and all(isinstance(d, str) and d.strip() for d in devices)):
        raise FindingError(f"{where}: devices must be a non-empty list of device tokens")
    if len(set(devices)) != len(devices):
        raise FindingError(f"{where}: devices lists a token twice")
    overrides = entry.get("set", {})
    if not isinstance(overrides, dict):
        raise FindingError(f"{where}: set must be a table")
    for key, value in overrides.items():
        types = SETTABLE.get(key)
        if types is None:
            raise FindingError(f"{where}: set.{key} is not settable (allowed: {', '.join(sorted(SETTABLE))})")
        # bool is an int subclass; an int field must not take true/false.
        if not isinstance(value, types) or (isinstance(value, bool) and bool not in types):
            raise FindingError(f"{where}: set.{key} must be {' or '.join(t.__name__ for t in types)}")
    action = entry["action"]
    if action in ("drop", "confirm") and overrides:
        raise FindingError(f"{where}: a {action} takes no set")
    if action == "change" and not overrides:
        raise FindingError(f"{where}: a change needs set")
    if "from" in entry and action != "add":
        raise FindingError(f"{where}: from applies to add only")
    if "group_of" in entry and action not in ("add", "define"):
        raise FindingError(f"{where}: group_of applies to add and define only")
    names = entry.get("name", {})
    if action == "define":
        missing = [k for k in DEFINE_REQUIRED if k not in overrides]
        if missing:
            raise FindingError(f"{where}: a define needs set.{', set.'.join(missing)}")
        if not (
            isinstance(names, dict)
            and set(names) == set(NAME_CULTURES)
            and all(isinstance(v, str) and v.strip() for v in names.values())
        ):
            raise FindingError(f"{where}: a define needs name = {{ {' = .., '.join(NAME_CULTURES)} = .. }}")
    elif "name" in entry:
        raise FindingError(f"{where}: name applies to define only; the export names the others")
    return Finding(
        tech=entry["tech"],
        action=action,
        devices=tuple(devices),
        checked=entry["checked"],
        what=entry["what"],
        unit=entry["unit"],
        source=entry["source"],
        set=dict(overrides),
        name=dict(names),
        from_device=entry.get("from", ""),
        group_of=entry.get("group_of", ""),
    )


def parse(doc: dict, origin: str = "<findings>") -> list[Finding]:
    """The findings of a parsed TOML document; FindingError on any defect.
    One (tech, device) pair may be named by one finding only, so the order of
    entries never decides the outcome."""
    unknown = sorted(set(doc) - {"finding"})
    if unknown:
        raise FindingError(f"{origin}: unknown top-level key(s) {', '.join(unknown)}")
    entries = doc.get("finding", [])
    if not isinstance(entries, list):
        raise FindingError(f"{origin}: finding must be an array of tables ([[finding]])")
    out = [_parse(e, f"{origin}: finding {i + 1}") for i, e in enumerate(entries)]
    seen: dict[tuple[str, str], int] = {}
    for i, f in enumerate(out):
        for dev in f.devices:
            prev = seen.setdefault((f.tech, dev), i)
            if prev != i:
                raise FindingError(f"{origin}: findings {prev + 1} and {i + 1} both name {f.tech} on {dev}")
    return out


def load(path: str = DEFAULT_PATH) -> list[Finding]:
    with open(path, "rb") as fh:
        return parse(tomllib.load(fh), os.path.basename(path))


def _same_definition(a: Event, b: Event) -> bool:
    keep = {f.name for f in dataclasses.fields(Event)} - {"id"}
    return all(getattr(a, k) == getattr(b, k) for k in keep)


def resolve_add_source(finding: Finding, candidates: list[Event], from_events: list[Event] | None) -> Event:
    """The export row an add links. *candidates* are every event with the
    finding's tech id; *from_events* the events of its ``from`` device."""
    pool = candidates
    if finding.from_device:
        if from_events is None:
            raise FindingError(f"{finding.tech}: from device {finding.from_device} is not in the export")
        pool = [e for e in from_events if e.tech == finding.tech]
    if not pool:
        if finding.from_device:
            raise FindingError(f"{finding.tech}: not on {finding.from_device} in the export")
        raise FindingError(f"{finding.tech}: no device in the export has this tech id; define it instead")
    if any(not _same_definition(pool[0], e) for e in pool[1:]):
        raise FindingError(f"{finding.tech}: the export defines it differently on several devices; name one with from")
    return pool[0]


def define_event(finding: Finding, culture: str, in_export: bool) -> Event:
    """The Event a define finding describes. *in_export*: some device in the
    export carries the tech id, which an add must reference instead."""
    if in_export:
        raise FindingError(f"{finding.tech}: the export defines it for another device; add it from there")
    s = finding.set
    return Event(
        id=f"finding:{finding.tech}",
        name=finding.name.get(culture) or finding.name["de"],
        address=s["address"],
        conversion=s["conversion"],
        access_type=s["access_type"],
        block_length=s["block_length"],
        byte_length=s["byte_length"],
        byte_position=s["byte_position"],
        bit_length=s.get("bit_length", 0),
        bit_position=s.get("bit_position", 0),
        tech=finding.tech,
        token=finding.tech,
        **{k: v for k, v in s.items() if k not in _EVENT_POSITIONAL},
    )


_EVENT_POSITIONAL = (
    "address",
    "conversion",
    "access_type",
    "block_length",
    "byte_length",
    "byte_position",
    "bit_length",
    "bit_position",
)


def _overlaps(a: Event, b: Event) -> bool:
    """Whether two datapoints read the same bytes the same way."""
    if a.address != b.address or (a.fc_read or "").lower() != (b.fc_read or "").lower():
        return False
    a0, b0 = a.byte_position or 0, b.byte_position or 0
    return a0 < b0 + (b.byte_length or 1) and b0 < a0 + (a.byte_length or 1)


def apply(events: list[Event], token: str, findings: list[Finding], add_source, define=None) -> list[Event]:
    """*events* of device *token* with *findings* applied. Changed and added
    events are copies; the shared export rows stay untouched. *add_source*
    maps an add finding to the export row it links (resolve_add_source),
    *define* a define finding to its new Event (define_event)."""
    mine = [f for f in findings if token in f.devices]
    if not mine:
        return events
    by_tech: dict[str, list[int]] = {}
    for i, ev in enumerate(events):
        by_tech.setdefault(ev.tech, []).append(i)
    out = list(events)
    dropped: set[int] = set()
    for f in mine:
        present = by_tech.get(f.tech, [])
        if f.action in ("add", "define"):
            if present:
                raise FindingError(f"{f.tech}: already on {token} in the export; the {f.action} is not needed there")
            group_id = ""
            if f.group_of:
                anchor = by_tech.get(f.group_of)
                if not anchor:
                    raise FindingError(f"{f.tech}: group_of {f.group_of} is not on {token}")
                group_id = events[anchor[0]].id
            if f.action == "add":
                new = dataclasses.replace(add_source(f), **f.set)
            else:
                if define is None:
                    raise FindingError(f"{f.tech}: define findings need a catalog")
                new = define(f)
                clash = next((ev for ev in events if _overlaps(ev, new)), None)
                if clash is not None:
                    raise FindingError(f"{f.tech}: {token} has {clash.tech} on the same bytes; confirm or change that")
            out.append(dataclasses.replace(new, group_of=group_id, confirmed=(f.label,), added=True))
            continue
        if not present:
            raise FindingError(f"{f.tech}: not on {token} in the export; nothing to {f.action}")
        for i in present:
            if f.action == "drop":
                dropped.add(i)
            else:
                out[i] = dataclasses.replace(out[i], **f.set, confirmed=(*out[i].confirmed, f.label))
    return [ev for i, ev in enumerate(out) if i not in dropped]
