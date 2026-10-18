"""Streaming Vitosoft XML parsers, text/unit resolution, and the Catalog class."""

from __future__ import annotations

from collections import defaultdict
import os
import re
import sys
import xml.etree.ElementTree as ET

import units

from .constants import _CONTAINER_PREFIXES, _CULTURES, _FACTOR_PLACEHOLDERS, _WANTED_ROWS
from .identify import generic_of, resolve_device, resolve_pool
from .models import Event, EventValue


def _local(tag: str) -> str:
    """Local tag name, dropping any '{namespace}' prefix ElementTree adds."""
    return tag.rsplit("}", 1)[-1]


def _child(elem, name: str) -> str | None:
    """Text of the first direct child whose LOCAL name matches (namespace-safe)."""
    for c in elem:
        if _local(c.tag) == name:
            return None if c.text is None else c.text.strip()
    return None


def _intval(s) -> int | None:
    if s is None or s == "":
        return None
    try:
        return int(s, 0)
    except (ValueError, TypeError):
        try:
            return int(s)
        except (ValueError, TypeError):
            return None


def _hx(s) -> int | None:
    if not s:
        return None
    try:
        return int(s, 16)
    except (ValueError, TypeError):
        return None


def _hexaddr(s) -> int | None:
    """Parse an access-layer address like '0x0886' (or '0886')."""
    if not s:
        return None
    try:
        return int(s.strip(), 16)
    except (ValueError, TypeError):
        return None


def _address_from_token(token: str) -> int | None:
    """Optolink address from a token like 'BetriebsstundenBrennerGWG~0x0886'."""
    if token and "~" in token:
        tail = token.rsplit("~", 1)[1]
        try:
            return int(tail, 16)
        except ValueError:
            return None
    return None


def _address_from_name(name: str) -> int | None:
    return _address_from_token(name)


def _strip_default_ns(text: str) -> str:
    """Drop the default xmlns so plain local-name lookups work everywhere."""
    return re.sub(r'\sxmlns="[^"]+"', "", text)


def _read_text(path: str) -> str:
    with open(path, "rb") as fh:
        raw = fh.read()
    enc = "utf-16" if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8"
    return raw.decode(enc, errors="replace")


def _xml_root_from_path(path: str):
    """Parse an XML file (UTF-8 or UTF-16) into an ElementTree root.

    The text is decoded first (_read_text), so its XML declaration -- which may
    name an encoding the decoded str no longer has -- is dropped before
    ET.fromstring. Namespaced tags are matched by local name (_local)."""
    text = _read_text(path)
    text = re.sub(r"^<\?xml[^>]*\?>", "", text, count=1).lstrip()
    return ET.fromstring(text)


def _find_file(data_dir: str, *names: str) -> str | None:
    for n in names:
        p = os.path.join(data_dir, n)
        if os.path.exists(p):
            return p
    return None


def _find_dpdefinitions(data_dir: str) -> str | None:
    hits = [
        os.path.join(dirpath, f)
        for dirpath, _dirs, files in os.walk(data_dir)
        for f in files
        if re.match(r"DPDefinitions.*\.xml$", f, re.IGNORECASE)
    ]
    return min(hits) if hits else None


# --- text / unit resolution ------------------------------------------------


def _load_textresources(path: str | None, culture: str = "de") -> dict[str, str]:
    """Build {Label: Value} from Textresource.xml, preferring *culture*."""
    if not path:
        return {}
    try:
        root = _xml_root_from_path(path)
    except ET.ParseError:
        return {}
    want = _CULTURES.get(culture, "1")
    primary: dict[str, str] = {}
    fallback: dict[str, str] = {}
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


def _overlay(base: dict, top: dict) -> dict:
    """base updated with top, except that an EMPTY value in top never hides a
    real one below it (English leaves e.g. ecnUnit.sech and many option labels
    empty)."""
    out = dict(base)
    for k, v in top.items():
        if v or k not in out:
            out[k] = v
    return out


def _load_text_layers(data_dir: str, culture: str = "de") -> tuple[dict, dict]:
    """(textmap, german) for *culture*, layered per label, lowest first:

    1. German: the slim ``Textresource.xml`` (~350 UI strings per culture), then
       ``Textresource_de.xml`` -- German is Vitosoft's base language and the
       only one every label exists in;
    2. the slim file's strings in *culture*;
    3. ``Textresource_<culture>.xml``.

    A label missing (or empty) in the chosen language therefore falls back to
    German for that one label, never to the raw key. ``german`` is layer 1
    alone, used where the text must not vary with the language (unit fallback).
    """
    slim_path = _find_file(data_dir, "Textresource.xml")
    german = _overlay(
        _load_textresources(slim_path, "de"),
        _load_textresources(_find_file(data_dir, "Textresource_de.xml"), "de"),
    )
    if culture == "de":
        return dict(german), german
    # The chosen language on top of German: its slim UI strings, then its full file.
    textmap = _overlay(german, _load_textresources(slim_path, culture))
    textmap = _overlay(
        textmap,
        _load_textresources(_find_file(data_dir, f"Textresource_{culture}.xml"), culture),
    )
    return textmap, german


def _lookup_label(raw: str, textmap: dict) -> str:
    """The Textresource text for an @@-prefixed label, or '' if there is none.

    DPDefinitions names an option ``@@viessmann.eventvaluetype.name.<X>~<N>``
    while the text files key it ``viessmann.eventvaluetype.<X>~<N>`` -- no
    ``.name.`` -- so both spellings are tried. Event names carry ``.name.`` in
    both places and resolve on the first try."""
    if not raw or not raw.startswith("@@"):
        return ""
    key = raw[2:]
    for k in (key, key.replace(".name.", ".", 1)):
        if textmap.get(k):
            return textmap[k]
    return ""


def _resolve_label(raw: str, textmap: dict) -> str:
    """Resolve a possibly @@-prefixed label. Without a text for it, trim the
    ``viessmann.<cat>.`` prefix so an enum option reads as ``Name~N`` rather
    than the full key."""
    if not raw:
        return raw
    if raw.startswith("@@"):
        hit = _lookup_label(raw, textmap)
        if hit:
            return hit
        key = raw[2:]
        return re.sub(r"^viessmann\.[a-z]+\.(name\.)?", "", key) or key
    return raw


def _resolve_unit(raw: str, german: dict) -> str:
    """The unit SYMBOL for a raw Vitosoft unit token (units.py). Never the
    translated label: that differs by language and is not a symbol. A token
    units.py does not know falls back to its German label, then to the bare
    token -- tests/unit/test_units.py keeps the table complete for the export."""
    if not raw:
        return ""
    row = units.lookup(raw)
    if row is not None:
        return row[0]
    key = raw.removeprefix("@@")
    if german.get(key):
        return german[key]
    if raw.startswith("ecnUnit."):
        return raw[len("ecnUnit.") :]
    return raw


def _readable(label: str) -> str:
    """Recover the Viessmann technical id from a display-name label (as the
    reference PrintEventsForDatapoint.py _readable does). Names are referenced
    as '@@viessmann.<type>.name.<TECH>'; without a text for that key, <TECH> is
    shown instead of the raw '@@...'. Already-resolved (non-@@) values pass
    through unchanged."""
    if not isinstance(label, str) or not label.startswith("@@"):
        return label
    s = label[2:]
    if ".name." in s:  # ...eventtype.name.Outside_Temp -> Outside_Temp
        return s.split(".name.", 1)[1]
    parts = s.split(".")  # ...eventvaluetype.K52_KonfiWeiche~0 -> K52_KonfiWeiche~0
    if len(parts) >= 3 and parts[0] in ("viessmann", "econtrolnet"):
        return ".".join(parts[2:])
    return s


def _friendly(label: str, textmap: dict | None = None) -> str:
    """Friendly entity name (as the reference PrintEventsForDatapoint.py
    _friendly does): a resolved translation verbatim, else the technical id with
    '_' -> spaces. Deliberately light -- camelCase / coding-prefix compounds
    (A1M1, K00_, WW, RT) are kept, so a real translation stays preferable where
    clean labels matter. The stable technical id is carried separately as the
    entity ``id:``."""
    if textmap and isinstance(label, str) and label.startswith("@@"):
        hit = textmap.get(label[2:])
        if hit:
            return hit
    if isinstance(label, str) and label and not label.startswith("@@"):
        # A real translation passes through; a raw "Tech~0xADDR" token (fixture
        # style) is humanised like the technical-id fallback.
        if "~0x" in label:
            return label.split("~", 1)[0].replace("_", " ").strip()
        return label
    return _readable(label).replace("_", " ").strip()


def _tech_id(name_field: str, token: str) -> str:
    """Stable technical identifier (the join key back to Viessmann; drives id:).
    Prefers the Address token stem -- the literal key into ecnEventType.xml --
    and falls back to the @@ display-name key only when no token is present."""
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


def _load_access(path: str | None) -> dict[str, dict]:
    """token (<ID>) -> access dict from ecnEventType.xml. {} if absent."""
    if not path:
        return {}
    acc: dict[str, dict] = {}
    for e in _xml_root_from_path(path).iter():
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
                "parameter_type": _child(e, "Parameter"),
            }
    return acc


def _load_identification(path: str | None) -> list[dict]:
    """List of identification rows from ecnDataPointType.xml. [] if absent."""
    if not path:
        return []
    rows: list[dict] = []
    for e in _xml_root_from_path(path).iter():
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
                    # Vitosoft's fault-code family: 1 Default (Vitotronic),
                    # 2 Vitocom, 3 heat pump, 4 BHKW, 6 WPR3, 7 Vitotwin; absent
                    # on M-Bus/GWG/gateway rows. Gates the generic fault texts.
                    "error_type": _child(e, "ErrorType"),
                }
            )
    return rows


# --- Catalog ---------------------------------------------------------------


class Catalog:
    """Parsed Vitosoft export: device types + events (merged with the access
    layer) + value types + identification."""

    generic_of = staticmethod(generic_of)

    def __init__(
        self,
        types: dict,
        dp_name: dict,
        links: dict,
        raw_events: dict,
        vlinks: dict,
        raw_values: dict,
        access: dict,
        ident_rows: list[dict],
        textmap: dict,
        groups: dict | None = None,
        glinks: dict | None = None,
        german: dict | None = None,
        culture: str = "de",
    ):
        self.devices = types  # token -> datapoint-type Id
        self._dp_name = dp_name  # Id -> display name
        self._links = links  # dp_id -> [event_id]
        self._vlinks = vlinks  # event_id -> [value_id]
        self._access = access  # token -> access dict
        self._ident = ident_rows
        self._text = textmap
        # German layers only: the unit fallback must not vary with the language.
        self._german = german if german is not None else textmap
        self._fault_index: tuple[dict, dict, dict] | None = None  # built on first use, see fault_texts()
        self.culture = culture
        # Vitosoft navigation tree (ecnEventTypeGroup): group id -> {"path":
        # readable "TOKEN~10_Bedienung_A1~20_Warmwasser", "dp": DataPointTypeId,
        # "order": OrderIndex}; glinks: event id -> [(group id, EventTypeOrder)].
        # Used by --order group and by the menu datapoints (_menu_events_by_dp).
        self._groups = groups or {}
        self._glinks = glinks or {}
        # Whether events_for() adds the datapoints a device carries only in its
        # own menu tree (see _menu_events_by_dp). On by default; the CLI's
        # --no-menu-datapoints turns it off.
        self.menu_datapoints = True
        self._menu_cache: dict | None = None
        self._menu_cache_sig: tuple[int, int] | None = None
        self._group_cache: dict = {}
        self._values = {vid: self._mk_value(f) for vid, f in raw_values.items()}
        self._events = {eid: self._mk_event(eid, f) for eid, f in raw_events.items()}

    def group_for(self, ev: Event, device_token: str) -> tuple[tuple, str]:
        """(sort_key, label) of the event's Vitosoft navigation group for this
        device, from ecnEventTypeGroup. The group's Address embeds the full
        readable path ("VScotHO1_72~10_Bedienung_A1~20_Warmwasser"); its
        numeric segment prefixes carry Vitosoft's intended ordering, so the raw
        segments are the sort key (all-str tuple; EventTypeOrder appended
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

    def _mk_value(self, f: dict) -> EventValue:
        return EventValue(
            name=_resolve_label(f.get("Name", ""), self._text),
            enum_address_value=_intval(f.get("EnumAddressValue")),
            enum_replace_value=_resolve_label(f.get("EnumReplaceValue", ""), self._text),
            description=(f.get("Description", "") or "").strip(),
            # German needs no translation: Description is German already, and
            # more specific than the UI label (see _enum_options).
            label="" if self.culture == "de" else _lookup_label(f.get("Name", ""), self._text),
            label_de=_lookup_label(f.get("Name", ""), self._german),
            unit=_resolve_unit(f.get("Unit", ""), self._german),
            unit_token=(f.get("Unit", "") or "").strip(),
            lower=f.get("LowerBorder", "") or "",
            upper=f.get("UpperBorder", "") or "",
            stepping=f.get("Stepping", "") or "",
        )

    def _mk_event(self, eid: str, f: dict) -> Event:
        token = f.get("Address", "") or ""
        acc = self._access.get(token, {})

        def pick(acc_key: str, dp_key: str) -> int | None:
            v = acc.get(acc_key)
            if v is None or v == "":
                v = f.get(dp_key)
            return _intval(v)

        def pick_str(acc_key: str, dp_key: str) -> str:
            """DPDefinitions first (fixture exports), else the access layer,
            which is where the current export keeps the borders."""
            v = f.get(dp_key, "") or ""
            if v == "":
                v = acc.get(acc_key) or ""
            return v

        raw_name = f.get("Name", "") or ""
        # Explicit None checks: 0x0000 is a valid Optolink address, and a
        # falsy-zero `or` chain would treat it as absent.
        addr = _hexaddr(acc.get("address"))
        if addr is None:
            addr = _address_from_token(token)
        if addr is None:
            addr = _address_from_name(raw_name)

        # value = raw * factor + offset. The export writes the placeholder pair
        # -0.1067 / 6992.58 on rows without a real transform, and
        # ConversionFactor 0 is its "no scaling" sentinel (common on
        # NoConversion rows) -- a literal 0x multiplier is never meaningful, so
        # both count as absent. A real 0 offset is fine and handled at the call
        # site.
        def _fval(acc_key: str, dp_key: str, *, is_factor: bool) -> float | None:
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

        values = [self._values[vid] for vid in self._vlinks.get(eid, ()) if vid in self._values]

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
            unit=_resolve_unit(acc.get("unit", ""), self._german),
            unit_token=(acc.get("unit", "") or "").strip(),
            tech=_tech_id(raw_name, token),
            token=token,
            fc_read=(acc.get("fc_read") or f.get("FCRead") or ""),
            fc_write=(acc.get("fc_write") or f.get("FCWrite") or ""),
            parameter_type=(acc.get("parameter_type") or f.get("Parameter") or "").strip(),
            conv_factor=_fval("conversion_factor", "ConversionFactor", is_factor=True),
            conv_offset=_fval("conversion_offset", "ConversionOffset", is_factor=False),
            block_factor=pick("block_factor", "BlockFactor"),
            values=values,
        )

    def _menu_events_by_dp(self) -> dict:
        """dp_id -> event ids in that device's OWN menu tree, sorted by id.

        Vitosoft attaches datapoints to a device in two places: the direct
        ecnDataPointTypeEventTypeLink table, and the device's menu tree --
        ecnEventTypeGroup rows carrying its DataPointTypeId, with datapoints hung
        on them by ecnEventTypeEventTypeGroupLink. Vitosoft shows a device
        through those menus, and many devices carry datapoints ONLY there (e.g.
        V200CO1x's whole (A0)..(B5) coding block, CU401B_S's compressor hours and
        annual performance factors). The menus are strictly per device: every
        group carries exactly one DataPointTypeId, and its path names that
        device (its own token, a variant of it, or
        ecnsysEventTypeGroup~<token>~...). Same idea as VExtractor's "take a
        controller's datapoints from its own menus too"; no code taken.

        Cached; rebuilt when the number of groups or of linked events changes."""
        sig = (len(self._groups), len(self._glinks))
        if self._menu_cache is None or self._menu_cache_sig != sig:
            by_dp: dict = defaultdict(set)
            for eid, links in self._glinks.items():
                for gid, _order in links:
                    g = self._groups.get(gid)
                    if g is not None and g.get("dp"):
                        by_dp[g["dp"]].add(eid)
            self._menu_cache = {
                dp: sorted(ids, key=lambda e: (len(e), e) if str(e).isdigit() else (0, str(e))) for dp, ids in by_dp.items()
            }
            self._menu_cache_sig = sig
        return self._menu_cache

    def events_for(self, device_token: str) -> list[Event]:
        """The device's datapoints: its direct links, then -- unless
        ``menu_datapoints`` is off -- those it carries only in its own menu
        tree (_menu_events_by_dp), each event once."""
        dp_id = self.devices.get(device_token)
        if dp_id is None:
            return []
        ids = list(self._links.get(dp_id, []))
        if self.menu_datapoints:
            seen = set(ids)
            ids += [e for e in self._menu_events_by_dp().get(dp_id, ()) if e not in seen]
        out: list[Event] = []
        for ev_id in ids:
            ev = self._events.get(ev_id)
            if ev is None:
                continue
            if not ev.values and ev_id in self._vlinks:
                ev.values = [self._values[vid] for vid in self._vlinks[ev_id] if vid in self._values]
            out.append(ev)
        return out

    def fault_texts(self) -> tuple[dict, dict, dict]:
        """(per_device, generic, gfa) fault-code texts from the loaded language:

        * per_device -- token -> {code: text}, ``viessmann.errorcode.<token>.<XX>``
        * generic    -- {code: text}, ``viessmann.errorcode.<XX>`` (Vitotronic)
        * gfa        -- chip -> {code: text}, ``viessmann.errorcode.fa.<chip>.<XX>``;
                        the chip is the byte GFA_Kennung (0x7650) reads, in hex

        The keys are hex code bytes; anything that is not one is skipped. Only
        the full Textresource_<culture>.xml files carry these; with the slim
        Textresource.xml alone all three are empty. Built once per catalog --
        --export-all asks for every device."""
        if self._fault_index is None:
            per_device: dict = defaultdict(dict)
            generic: dict = {}
            gfa: dict = defaultdict(dict)
            prefix = "viessmann.errorcode."
            for label, text in self._text.items():
                if not label.startswith(prefix) or not (text or "").strip():
                    continue
                parts = label[len(prefix) :].split(".")
                try:
                    code = int(parts[-1], 16)
                except ValueError:
                    continue
                if not 0 <= code <= 0xFF:
                    continue
                text = text.strip()
                if len(parts) == 1:
                    generic[code] = text
                elif len(parts) == 2:
                    per_device[parts[0]][code] = text
                elif len(parts) == 3 and parts[0] == "fa":
                    gfa[parts[1].upper()][code] = text
            self._fault_index = (dict(per_device), generic, dict(gfa))
        return self._fault_index

    def identification_for(self, token: str) -> dict:
        for r in self._ident:
            if r.get("token") == token:
                return r
        return {}

    def resolve_pool(self, device_id: int, hw: int | None, sw: int):
        """See identify.resolve_pool."""
        return resolve_pool(self._ident, device_id, hw, sw)

    def resolve(self, device_id: int, hw: int, sw: int, f0: int | None = None):
        """See identify.resolve_device."""
        return resolve_device(self._ident, device_id, hw, sw, f0)


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
    textmap, german = _load_text_layers(data_dir, culture)
    if not access:
        print(
            "warning: ecnEventType.xml not found next to DPDefinitions.xml; "
            "lengths/bit masks/addresses may be incomplete (the 2026 export keeps "
            "them there, not in DPDefinitions.xml).",
            file=sys.stderr,
        )

    types: dict = {}
    dp_name: dict = {}
    links: dict = defaultdict(list)
    vlinks: dict = defaultdict(list)
    raw_events: dict = {}
    raw_values: dict = {}
    groups: dict = {}
    glinks: dict = defaultdict(list)

    context = ET.iterparse(dp_path, events=("start", "end"))
    _, root = next(context)

    for event, el in context:
        if event != "end":
            continue
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
            # Free the finished row / dataset, and drop finished children from
            # the root so it does not grow with the file.
            el.clear()
            if len(root) > 1000:
                del root[:-1]

    return Catalog(
        types,
        dp_name,
        links,
        raw_events,
        vlinks,
        raw_values,
        access,
        ident,
        textmap,
        groups=groups,
        glinks=glinks,
        german=german,
        culture=culture,
    )
