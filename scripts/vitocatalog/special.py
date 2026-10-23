"""Special multi-slot and hub-fed entities: device_id, Schaltzeiten, Error History, GFA companions."""

from __future__ import annotations

import re

import fault_codes

from .constants import (
    _ERROR_HISTORY_ADDRESS,
    _IDENT_ADDRESSES,
    _SCHALTZEITEN_BLOCK_LEN,
    _SCHALTZEITEN_DAYS,
    _SCHALTZEITEN_RECORD_LEN,
    _WEEKDAYS,
    GFA_DIRNAME,
    HYDRO_SCHEDULE_MAPPINGS,
    MAPPING_PHASE_CT,
    MAX_P300_READ_LENGTH,
    POLL_ERROR,
    _ui,
)
from .models import Event


def _yaml_str(s: str) -> str:
    # Always double-quote and escape, so umlauts / specials never break parsing.
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _is_identification(ev: Event) -> bool:
    return ev.address in _IDENT_ADDRESSES


def _device_id_lines(oid: str, lang: str = "de") -> list[str]:
    """The hub-fed device identity text_sensor (no address)."""
    return [
        "- platform: vitohome",
        "  type: device_id",
        f"  name: {_yaml_str(_ui(lang, 'device_type'))}",
        f"  id: {oid}",
        '  icon: "mdi:identifier"',
        "  entity_category: diagnostic",
    ]


# --- Schaltzeiten (56-byte weekday switching programs) ----------------------


def _is_schaltzeiten(ev: Event) -> bool:
    """A weekday switching-time program the component's `text` platform can
    decode: Vitosoft reads it as PhaseCT (MappingType 1) AND the access layer
    declares the 56-byte / BlockFactor-7 / 8-byte-record shape. That takes in
    the programs not named Schaltzeiten (Vitotwin_2100_Zeitprogramm_HK1 and
    siblings). Without a MappingType (older exports) the token must name a
    Schaltzeiten block instead. Other shapes and mappings (the heat-pump
    programs, _hydro_schedule_shape; LON group, pre-decomposed KBUS) return
    False."""
    if ev.address is None:
        return False
    mapping = getattr(ev, "mapping_type", None)
    if mapping:
        if mapping != MAPPING_PHASE_CT:
            return False
    else:
        tok = getattr(ev, "token", "") or getattr(ev, "tech", "") or getattr(ev, "name", "") or ""
        if "Schaltzeit" not in tok:
            return False
    blen = ev.byte_length or ev.block_length or 0
    bf = getattr(ev, "block_factor", 0) or 0
    return blen == _SCHALTZEITEN_BLOCK_LEN and bf == _SCHALTZEITEN_DAYS


def _hydro_schedule_shape(ev: Event) -> tuple[int, int] | None:
    """(records per day, record length) of a heat-pump weekly program, or None.

    Vitosoft reads these with a Hydro* MappingType (HYDRO_SCHEDULE_MAPPINGS):
    168 bytes in BlockFactor records, one ADDRESS per record -- the export's
    next program starts BlockFactor addresses on, not 168. So a day is
    BlockFactor / 7 records starting at base + day * (BlockFactor / 7): one
    24-byte record per address for HydroCT (0x9000, 0x9001, ...), eight 3-byte
    records for HydroPhaseCT (0x9200, 0x9208, ...). None for any other
    mapping, a shape that does not split into 7 days of whole records, or a
    day wider than one read."""
    if ev.address is None or getattr(ev, "mapping_type", None) not in HYDRO_SCHEDULE_MAPPINGS:
        return None
    blen = ev.block_length or ev.byte_length or 0
    bf = getattr(ev, "block_factor", 0) or 0
    if blen <= 0 or bf <= 0 or blen % bf or bf % _SCHALTZEITEN_DAYS:
        return None
    per_day, record_len = bf // _SCHALTZEITEN_DAYS, blen // bf
    return (per_day, record_len) if per_day * record_len <= MAX_P300_READ_LENGTH else None


def _hydro_schedule_entries(ev: Event, lang: str = "de") -> list[dict]:
    """One read-only raw entity per day of a heat-pump weekly program (see
    _hydro_schedule_shape), Monday first: dicts with address, length, name,
    seed and the comment line naming the shape."""
    per_day, record_len = _hydro_schedule_shape(ev)
    stem = (getattr(ev, "token", "") or getattr(ev, "tech", "") or "").split("~", 1)[0]
    seed_prog = re.sub(r"[^a-z0-9]+", "_", stem.lower()).strip("_")
    base = ev.address or 0
    mapping = HYDRO_SCHEDULE_MAPPINGS[ev.mapping_type]
    shape = f"{per_day} x {record_len}-byte records" if per_day > 1 else f"one {record_len}-byte record"
    comment = [
        f"# Vitosoft {mapping} weekly program: {shape} per day, one",
        f"# address per record (day n at 0x{base:04X} + n*{per_day}). Raw hex: the export",
        "# does not describe the record layout.",
    ]
    entries: list[dict] = []
    for day, (_label, sfx) in enumerate(_WEEKDAYS):
        entries.append(
            {
                "address": base + day * per_day,
                "length": per_day * record_len,
                "name": f"{ev.name or stem} {_ui(lang, 'weekdays')[day]}",
                "seed": f"{seed_prog}_{sfx}",
                "comment": comment,
            }
        )
    return entries


def _hydro_schedule_lines(entry: dict, oid: str, access: str = "") -> list[str]:
    """YAML lines for one day of a heat-pump weekly program: a read-only raw
    `text_sensor`, polled hourly like the boiler programs."""
    lines = [
        *entry["comment"],
        "- platform: vitohome",
        "  type: raw",
        f"  name: {_yaml_str(entry['name'])}",
        f"  id: {oid}",
        f"  address: 0x{entry['address']:04X}",
    ]
    if access:
        lines.append(f"  access: {access}")
    lines += [
        f"  length: {entry['length']}",
        "  entity_category: diagnostic",
        "  disabled_by_default: true",
        "  update_interval: 3600s",
    ]
    return lines


def _schaltzeiten_program_label(ev: Event) -> str:
    """Human program label from the token: 'Schaltzeiten_A1M1_HK' -> 'A1M1 HK'.
    Drops the leading 'Schaltzeiten' word, underscores -> spaces."""
    tok = (getattr(ev, "token", "") or getattr(ev, "tech", "") or "").split("~", 1)[0]
    label = re.sub(r"^Schaltzeiten_?", "", tok).replace("_", " ").strip()
    return label or tok


def _schaltzeiten_entries(ev: Event, lang: str = "de") -> list[dict]:
    """Expand one 56-byte Schaltzeiten block into 7 per-day `text` specs at
    base + day*8 (Monday first), matching the component's decode. Returns dicts
    with: address, name, seed, program (the shared program label)."""
    prog = _schaltzeiten_program_label(ev)
    seed_prog = re.sub(r"[^a-z0-9]+", "_", prog.lower()).strip("_")
    base = ev.address or 0
    entries: list[dict] = []
    for day, (_label, sfx) in enumerate(_WEEKDAYS):
        label = _ui(lang, "weekdays")[day]
        entries.append(
            {
                "address": base + day * _SCHALTZEITEN_RECORD_LEN,
                "name": f"{_ui(lang, 'switching_time')} {prog} {label}",
                "seed": f"schaltzeit_{seed_prog}_{sfx}",
                "program": prog,
            }
        )
    return entries


def _schaltzeiten_lines(entry: dict, oid: str, access: str = "") -> list[str]:
    """YAML lines for one per-day Schaltzeiten `text` entity. Read and write use
    the same weekday address (the component re-reads what it wrote); polled
    hourly and disabled by default like every generated entity.

    *access* is the GWG access mode (empty on every other protocol). A GWG
    address is a datapoint identity only together with its mode, so the text
    entity carries it like every other entity; without it the engine would read
    and write on its PHYSICAL default whatever FCRead said."""
    lines = [
        "- platform: vitohome",
        f"  name: {_yaml_str(entry['name'])}",
        f"  id: {oid}",
        f"  address: 0x{entry['address']:04X}",
    ]
    if access:
        lines.append(f"  access: {access}")
    lines += [
        "  disabled_by_default: true",
        "  update_interval: 3600s",
    ]
    return lines


# --- Error history & GFA fault archives -------------------------------------


def _is_error_history(ev: Event) -> bool:
    """An error-history slot: the generic ecnsysEventType~Error block, OR a
    device-specific FehlerHis* slot, OR the 0x7507 address fallback. The
    'Index'/pointer slot is excluded.

    These are TWO DIFFERENT archives, not two addresses for one, and a unit can
    carry both (VScotHO1_72 does):
      * ecnsysEventType~Error at 0x7507 (length 90): the 10-slot x 9-byte
        Vitotronic SYSTEM fault history -- the one the Bedienteil shows and
        vcontrold's getError0..9 read;
      * FehlerHisFA01..20 (0x7590..0x763B on VScotHO1_72): the
        Feuerungsautomat (GFA burner control unit) history, a different
        subsystem with a different code space (GFA_Kennung sits at 0x7650, K38
        is KonfiFehlerByteGFA).
    See _error_history_entries for the emission rules."""
    if ev.address is None:
        return False
    tok = getattr(ev, "token", "") or getattr(ev, "tech", "") or getattr(ev, "name", "") or ""
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
    tok = getattr(ev, "token", "") or getattr(ev, "tech", "") or getattr(ev, "name", "") or ""
    m = _FEHLERHIS_SLOT_RE.search(tok)
    return int(m.group(1)) if m else None


def _error_history_archive_tag(ev: Event) -> str:
    """A short tag distinguishing one system archive from another on units that
    carry MORE THAN ONE (e.g. the Vitotwin gateway has both
    ecnsysEventType~Error @ 0x7507 and ecnsysEventType~VitotwinErrorHistorySW02
    @ 0x7000). The canonical archive returns "", so its entity names stay
    "Letzter Fehler" / "Fehler NN"; a distinct secondary archive contributes a
    suffix so the two do not collide (ESPHome requires unique names per
    platform).

    Only a ``prefix~subname`` token whose subname is NOT the canonical
    ``Error`` is tagged: a bare token with no ``~`` (the fixture / 0x7507
    address-fallback style) is the single canonical archive and stays untagged.
    """
    tok = getattr(ev, "token", "") or getattr(ev, "tech", "") or ""
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


def _error_history_entries(ev: Event, lang: str = "de", rpc_records: tuple[int, int] | None = None) -> list[dict]:
    """Expand one error-history event into per-slot emission specs.

    Returns dicts with: address, name, seed, disabled, system (bool), gfa
    (FehlerHis* slots only), comment (list of lines). Rules:
    - FehlerHisFA* -> ONE slot of the Feuerungsautomat (GFA burner control
      unit) history: named "GFA Fehler NN", disabled by default, and flagged
      system=False so the Vitotronic fault-code map is NOT attached (the GFA
      lockout codes are a different code space).
    - ecnsysEventType~Error -> its BlockFactor slot count (the access layer
      declares BlockLength 90 / BlockFactor 10 -> ten 9-byte system slots at
      base + i*stride; vcontrold getError0..9). A >=90-byte block without
      BlockFactor is taken as 10 slots of 9 bytes. The per-slot STRIDE is
      BlockLength // BlockFactor (9 for the standard archive, 12 for the
      Vitotwin's 120-byte archive); the component reads the leading 9 bytes
      (code + 8-byte BCD) at each slot address. Slot 1 is "Letzter Fehler"
      (newest first), slots 2..N are disabled.
    - a SECOND system archive on the same unit gets a short tag appended to its
      names/seeds so they do not collide with the first (see
      _error_history_archive_tag).
    - any other system-history event (single slot, e.g. the 0x7507 address
      fallback) -> one enabled "Letzter Fehler" slot at its own address.
    - ``rpc_records`` (count, length) from transforms._rpc_array_records -> the
      same slots, named and enabled the same way, but every one at the archive's
      own address with its record index as the call's parameter (``prefix``):
      the heat-pump fault buffers are an RPC array, not a run of addresses. The
      record layout is not decoded yet, so these slots are raw hex (``raw``).
    """
    if rpc_records is not None:
        count, length = rpc_records
        tag = _error_history_archive_tag(ev)
        name_suffix = f" ({tag})" if tag else ""
        seed_suffix = "_" + re.sub(r"[^a-z0-9]+", "_", tag.lower()).strip("_") if tag else ""
        token = getattr(ev, "token", "") or "ecnsysEventType~Error"
        return [
            {
                "address": ev.address,
                "prefix": [i],
                "length": length,
                "raw": True,
                "name": (_ui(lang, "last_fault") if i == 0 else f"{_ui(lang, 'fault')} {i + 1:02d}") + name_suffix,
                "seed": ("letzter_fehler" if i == 0 else f"fehler_{i + 1:02d}") + seed_suffix,
                "disabled": i > 0,
                "system": True,
                "comment": [
                    f"# Fault history record {i + 1} of {count} ({token}, a {count} x {length}-byte",
                    f"# remote procedure array at 0x{ev.address:04X}: one call per record, the",
                    f"# record index {i} as the parameter). Raw hex: the record layout is not",
                    "# decoded yet." + (" Index 0 is taken as the newest; unconfirmed on hardware." if i == 0 else ""),
                ],
            }
            for i in range(count)
        ]
    fa_slot = _error_history_slot(ev)
    if fa_slot is not None:
        # Some units carry SEVERAL FehlerHis-style archives that all match the
        # slot regex: Vitovalor has FehlerHisFA01..20 (the GFA burner archive),
        # Fehlerhistorie_FCU_0..9 and Fehlerhist_FCU_0..9 (two fuel-cell-unit
        # archives). A family key from the token stem (everything before the
        # slot digits) gives distinct families distinct names/seeds; the
        # canonical FehlerHisFA family stays plain "GFA Fehler NN". The stems
        # are already distinct across families, so the stem is used verbatim
        # (underscores -> spaces): shortening them to a common "FCU" would make
        # the names collide.
        tok = getattr(ev, "token", "") or getattr(ev, "tech", "") or ""
        stem = re.split(r"\d", tok.split("~", 1)[0], maxsplit=1)[0].rstrip("_")
        if re.fullmatch(r"FehlerHisFA", stem, re.IGNORECASE) or stem == "":
            fam_name = "GFA"
            fam_seed = "gfa"
        else:
            fam_name = stem.replace("_", " ").strip() or stem
            fam_seed = re.sub(r"[^a-z0-9]+", "_", stem.lower()).strip("_")
        return [
            {
                "address": ev.address,
                "name": f"{fam_name} {_ui(lang, 'fault')} {fa_slot:02d}",
                "seed": f"{fam_seed}_fehler_{fa_slot:02d}",
                "disabled": True,
                "system": False,
                "gfa": fam_name == "GFA",
                "comment": [
                    f"# {fam_name} history slot {fa_slot} (a burner/subsystem fault archive",
                    "# DISTINCT from the Vitotronic system history). Its lockout codes are",
                    "# a different code space from the F-codes, keyed by the burner-control",
                    "# chip (--gfa-chip); without it the code byte displays as raw hex. On VScotHO1_72 the GFA slot 1",
                    "# was HARDWARE-CONFIRMED distinct (read 0x00, its own timestamp 42 s",
                    "# after the system slot's 0x38). No public GFA code enumeration is",
                    "# known (export + community search both empty).",
                ],
            }
        ]
    blen = ev.byte_length or ev.block_length or 9
    # Slot structure from the access layer where present: BlockLength =
    # BlockFactor records. The stride is the record size; the component reads
    # only the leading 9 bytes (code + 8-byte BCD timestamp) of each, so a
    # 12-byte-record archive (Vitotwin, 120/10) still advances by 12. Without a
    # BlockFactor, a >=90-byte block is 10 slots of 9 bytes.
    bf = getattr(ev, "block_factor", 0) or 0
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
                    "name": (_ui(lang, "last_fault") if slot == 1 else f"{_ui(lang, 'fault')} {slot:02d}") + name_suffix,
                    "seed": ("letzter_fehler" if slot == 1 else f"fehler_{slot:02d}") + seed_suffix,
                    "disabled": slot > 1,
                    "system": True,
                    "comment": [
                        f"# System fault history slot {slot} of {slots} ({getattr(ev, 'token', '') or 'ecnsysEventType~Error'},",
                        f"# {blen}-byte block at 0x{ev.address:04X} + {i}*{stride}; vcontrold getError{i}).",
                    ]
                    + (["# Slot 1 = newest entry -- hardware-confirmed on VScotHO1_72."] if slot == 1 else []),
                }
            )
        return entries
    return [
        {
            "address": ev.address,
            "name": _ui(lang, "last_fault") + name_suffix,
            "seed": "letzter_fehler" + seed_suffix,
            "disabled": False,
            "system": True,
            "comment": [
                "# System fault history, most-recent slot (code byte + 8-byte BCD",
                "# timestamp).",
            ],
        }
    ]


def _codes_lines(codes: dict, note: str) -> list[str]:
    """The ``codes:`` block of one error-history slot; see _error_history_lines."""
    return [f"  # {note}", "  codes:"] + [f"    0x{code:02X}: {_yaml_str(text)}" for code, text in sorted(codes.items())]


def _error_history_lines(
    entry: dict,
    oid: str,
    codes_map: dict | None,
    codes_note: str = "",
    gfa_map: dict | None = None,
    gfa_note: str = "",
    access: str = "",
    fc_read: str = "",
) -> list[str]:
    """One error-history slot (from _error_history_entries) as a 9-byte block
    (code byte + 8-byte BCD timestamp), decoded by the component's
    error_history type. ``codes_map`` is attached only to SYSTEM slots;
    ``gfa_map`` only to the GFA burner-control slots (a different code space,
    keyed by the GFA chip -- see _resolve_gfa_codes). ``*_note`` is the comment
    stating where the texts come from. Repeated maps become YAML aliases later,
    in output order (_share_codes_blocks). ``access`` / ``fc_read`` are the GWG
    access mode and its Vitosoft FCRead (empty on other protocols)."""
    lines = list(entry["comment"])
    if entry.get("raw"):
        # A record of an RPC array (_error_history_entries, rpc_records): the
        # call's parameter is the record index, the answer is shown as raw hex.
        prefix = ", ".join(f"0x{b:02X}" for b in entry["prefix"])
        lines += [
            "- platform: vitohome",
            "  type: raw",
            f"  name: {_yaml_str(entry['name'])}",
            f"  id: {oid}",
            f"  address: 0x{entry['address']:04X}",
            f"  access: rpc  # Vitosoft {fc_read}",
            f"  prefix: [{prefix}]  # the record index",
            f"  length: {entry['length']}",
            '  icon: "mdi:alert-circle"',
            "  entity_category: diagnostic",
        ]
        if entry["disabled"]:
            lines.append("  disabled_by_default: true")
        lines.append(f"  update_interval: {POLL_ERROR}s")
        return lines
    lines += [
        "# If the device NAKs the 9-byte block read, drop length to 1 (code only).",
        "- platform: vitohome",
        "  type: error_history",
        f"  name: {_yaml_str(entry['name'])}",
        f"  id: {oid}",
        f"  address: 0x{entry['address']:04X}",
    ]
    if access:
        lines.append(f"  access: {access}  # Vitosoft {fc_read}")
    lines += [
        "  length: 9",
        '  icon: "mdi:alert-circle"',
        "  entity_category: diagnostic",
    ]
    if entry["disabled"]:
        lines.append("  disabled_by_default: true")
    lines.append(f"  update_interval: {POLL_ERROR}s")
    if entry["system"] and codes_map:
        lines += _codes_lines(codes_map, codes_note or "Fault-code map:")
    elif entry["system"]:
        lines.append('  # codes: { 0x00: "kein Fehler", ... }  # add a code->text map')
    elif entry.get("gfa") and gfa_map:
        lines += _codes_lines(gfa_map, gfa_note)
    elif entry.get("gfa") and gfa_note:
        lines.append(f"  # {gfa_note}")
    return lines


def _resolve_fault_codes(catalog, device: str, error_codes: bool, error_code_set: str):
    """(codes_map, note) for the SYSTEM fault history of *device*.

    ``auto`` (the default) and ``vitosoft`` take Vitosoft's own texts from the
    full Textresource_<culture>.xml, in the catalog language:

    1. the device's own set (``viessmann.errorcode.<token>.*``), which most
       catalogs with a system history have;
    2. else the generic Vitotronic set, but only for ``ErrorType`` 1 (Default):
       it is the boiler meaning, wrong for a BHKW (ErrorType 4) or heat pump;
    3. else nothing -- ``auto`` then falls back to the hand-curated vd300 map,
       but only when the export carries no fault texts at all (slim
       Textresource).

    An explicit fault_codes set name (vd300, vd200, openv, union) is used as is.
    """
    if not error_codes:
        return None, ""
    if error_code_set in fault_codes.SETS:
        return (
            fault_codes.SETS[error_code_set],
            f"Fault-code map ('{error_code_set}' set) - DEFAULT, verify for this unit (see fault_codes.CONFLICTS):",
        )
    per_device, generic, _gfa = catalog.fault_texts()
    if device in per_device:
        return per_device[device], f"Fault-code texts: Vitosoft, for this device (viessmann.errorcode.{device}.*)"
    if generic and catalog.identification_for(device).get("error_type") == "1":
        return generic, "Fault-code texts: Vitosoft generic Vitotronic set (no device-specific set) - verify"
    if error_code_set == "auto" and not (per_device or generic):
        return (
            fault_codes.SETS["vd300"],
            "Fault-code map ('vd300' set) - this export has no fault texts (slim Textresource); verify for this unit:",
        )
    return None, ""


def _resolve_gfa_codes(catalog, gfa_chip: str | None):
    """(gfa_map, note) for the GFA burner-control history slots.

    The texts are keyed by the burner control's chip, which the boiler reports
    in GFA_Kennung (0x7650, one byte; every device that has it uses that
    address). It is a property of the installed unit, not of the device type,
    so it cannot come from the export: pass the byte you read as --gfa-chip.
    The chip's name comes from Vitosoft's own table (eventvaluetype
    Kennung_FA_Chip~<decimal>: 0x24 = "24h - NEC")."""
    _per, _gen, gfa = catalog.fault_texts()
    if not gfa:
        return None, ""
    if not gfa_chip:
        return None, (
            "GFA texts: include gfa/chip_<XX>.yaml for the GFA=0x<XX> the hub logs at boot "
            "(or regenerate with --gfa-chip <XX>)"
        )
    chip = gfa_chip.upper().removeprefix("0X").zfill(2)
    if chip not in gfa:
        known = ", ".join(sorted(gfa))
        raise SystemExit(f"--gfa-chip {gfa_chip}: no GFA texts for chip {chip} (known: {known})")
    name = catalog._text.get(f"viessmann.eventvaluetype.Kennung_FA_Chip~{int(chip, 16)}", "")
    label = f"{chip} ({name.split(' - ', 1)[-1]})" if name else chip
    return gfa[chip], f"GFA fault texts: Vitosoft, burner-control chip {label} (viessmann.errorcode.fa.{chip}.*)"


# --- GFA companions ---------------------------------------------------------
# The GFA fault texts depend on the chip the installed unit reports at 0x7650,
# not on the device type, so a catalog cannot carry them. A companion package
# does: one small file per chip, which extends the catalog's GFA history slots
# by id -- instead of a per-chip copy of every GFA catalog.


def gfa_companion_name(chip: str) -> str:
    return f"chip_{chip.upper()}.yaml"


def gfa_chip_names(catalog) -> dict[str, str]:
    """{chip: name} for every chip with fault texts, from Vitosoft's own chip
    table (eventvaluetype Kennung_FA_Chip~<decimal>: "24h - NEC" -> "NEC"). Read
    from the German texts so resolve.json does not differ between languages."""
    _per, _gen, gfa = catalog.fault_texts()
    german = getattr(catalog, "_german", catalog._text)
    out = {}
    for chip in sorted(gfa):
        label = german.get(f"viessmann.eventvaluetype.Kennung_FA_Chip~{int(chip, 16)}", "")
        out[chip] = label.split(" - ", 1)[-1].strip() if label else ""
    return out


def gfa_companion(catalog, chip: str, slot_ids: list[str]) -> str:
    """The companion package for one chip: every GFA slot id gets the chip's
    codes via ``id: !extend``. The map is written once under a YAML anchor and
    reused by alias; the component emits identical tables once
    (emit_option_table), so the slots share one table in flash too."""
    _per, _gen, gfa = catalog.fault_texts()
    codes = gfa[chip]
    name = gfa_chip_names(catalog).get(chip, "")
    label = f"{chip} ({name})" if name else chip
    out = [
        "# ============================================================",
        f"# GFA burner-control fault texts, chip {label} -- companion package",
        "#",
        "# Include NEXT TO a catalog that has the GFA fault history",
        f"# ({slot_ids[0]} .. {slot_ids[-1]}); it adds the texts to those entities:",
        "#",
        "#   packages:",
        "#     heater: !include catalogs/<your-unit>.yaml",
        f"#     gfa: !include catalogs/{GFA_DIRNAME}/{gfa_companion_name(chip)}",
        "#",
        "# The hub logs the chip at boot, in its identification line and the",
        '# catalog-finder link ("GFA=0x24 (NEC)", "&gfa=24"; read from 0x7650,',
        "# GFA_Kennung). The texts differ by chip, and 7 codes mean different things",
        "# on different chips, so use the one your unit reports.",
        "#",
        f"# Generated by scripts/gen_catalog.py from Vitosoft (viessmann.errorcode.fa.{chip}.*).",
        "# ============================================================",
        "",
        "text_sensor:",
    ]
    for i, oid in enumerate(slot_ids):
        out.append(f"  - id: !extend {oid}")
        if i == 0:
            out.append("    codes: &fault_codes")
            out += [f"      0x{code:02X}: {_yaml_str(text)}" for code, text in sorted(codes.items())]
        else:
            out.append("    codes: *fault_codes")
    return "\n".join(out) + "\n"
