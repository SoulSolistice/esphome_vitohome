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
    action    "add" (link an existing Vitosoft datapoint to the device),
              "change" (override fields of one the device has) or
              "drop" (remove it from the device)
    devices   exact device tokens, e.g. ["GWG_VBES_03"]
    checked   "read" (values read and matched the control panel or a gauge)
              or "write" (written and read back)
    what      one line: what the unit showed
    unit      how the unit identified itself, e.g. "0x2053 HW=0x01 SW=0x1E"
    source    where the evidence is, e.g. "SoulSolistice/esphome_vitohome#35"
    set       (add, change) Event fields to override, see SETTABLE
    from      (add) device token to take the row from when the export carries
              the tech id in differing definitions
    group_of  (add) tech id of a datapoint on the device whose menu group the
              added one joins; without it the entity is ungrouped

stdlib only (tomllib), like the rest of the generator.
"""

from __future__ import annotations

import dataclasses
from dataclasses import dataclass, field
import os
import tomllib

from .models import Event

DEFAULT_PATH = os.path.join(os.path.dirname(__file__), "confirmed.toml")

ACTIONS = ("add", "change", "drop")
CHECKS = ("read", "write")
_REQUIRED = ("tech", "action", "devices", "checked", "what", "unit", "source")
_OPTIONAL = ("set", "from", "group_of")

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
    from_device: str = ""
    group_of: str = ""

    @property
    def label(self) -> str:
        """The finding as one line, rendered above the entity."""
        return f"{self.what} [{self.checked} on {self.unit}; {self.source}]"


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
    if action == "drop" and overrides:
        raise FindingError(f"{where}: a drop takes no set")
    if action == "change" and not overrides:
        raise FindingError(f"{where}: a change needs set")
    if action != "add" and ("from" in entry or "group_of" in entry):
        raise FindingError(f"{where}: from and group_of apply to add only")
    return Finding(
        tech=entry["tech"],
        action=action,
        devices=tuple(devices),
        checked=entry["checked"],
        what=entry["what"],
        unit=entry["unit"],
        source=entry["source"],
        set=dict(overrides),
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
        where = f" on {finding.from_device}" if finding.from_device else ""
        raise FindingError(f"{finding.tech}: no Vitosoft datapoint with this tech id{where} to add")
    if any(not _same_definition(pool[0], e) for e in pool[1:]):
        raise FindingError(f"{finding.tech}: the export defines it differently on several devices; name one with from")
    return pool[0]


def apply(events: list[Event], token: str, findings: list[Finding], add_source) -> list[Event]:
    """*events* of device *token* with *findings* applied. Changed and added
    events are copies; the shared export rows stay untouched. *add_source*
    maps an add finding to the export row it links (resolve_add_source)."""
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
        if f.action == "add":
            if present:
                raise FindingError(f"{f.tech}: already on {token} in the export; the add is not needed there")
            group_id = ""
            if f.group_of:
                anchor = by_tech.get(f.group_of)
                if not anchor:
                    raise FindingError(f"{f.tech}: group_of {f.group_of} is not on {token}")
                group_id = events[anchor[0]].id
            src = add_source(f)
            out.append(dataclasses.replace(src, **f.set, group_of=group_id, confirmed=(f.label,)))
            continue
        if not present:
            raise FindingError(f"{f.tech}: not on {token} in the export; nothing to {f.action}")
        for i in present:
            if f.action == "drop":
                dropped.add(i)
            else:
                out[i] = dataclasses.replace(out[i], **f.set, confirmed=(*out[i].confirmed, f.label))
    return [ev for i, ev in enumerate(out) if i not in dropped]
