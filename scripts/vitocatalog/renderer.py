"""Name deduplication, YAML anchor sharing, and full package generation."""

from __future__ import annotations

import re

from .constants import _SCHALTZEITEN_RECORD_LEN, _WEEKDAYS, ACCESS_PLATFORMS, PROTO_GWG, PROTO_P300, RPC_PLATFORMS
from .emitter import _mode_select_caveat, build_entity_spec, render_entity_lines
from .loader import Catalog
from .models import CommentSpec, Event
from .special import (
    _device_id_lines,
    _error_history_entries,
    _error_history_lines,
    _error_history_slot,
    _is_error_history,
    _is_identification,
    _is_schaltzeiten,
    _resolve_fault_codes,
    _resolve_gfa_codes,
    _schaltzeiten_entries,
    _schaltzeiten_lines,
)
from .transforms import (
    _access_for,
    _is_diagnostic,
    _is_lon_bound_rpc,
    _is_lon_input,
    _is_mode_select,
    _is_reachable,
    _is_writable,
    _profile_keep,
    _protocol_for_device,
    _read_only_blocks,
    _rpc_prefix,
)


def _make_obj_id(tech: str, used: set[str]) -> str:
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


def _esphome_object_id(name: str) -> str:
    """The id ESPHome derives from an entity name -- and compares for
    uniqueness -- per esphome.helpers: lower-case, spaces to "_", anything
    outside [a-z0-9_-] to "_". So "Pegel" and "pegel", or two names differing
    only in an umlaut, collide."""
    return re.sub(r"[^a-z0-9_-]", "_", (name or "").lower().replace(" ", "_"))


_NAME_LINE = re.compile(r'^(\s*name: )"(.*)"$')
_ID_LINE = re.compile(r"^\s*id: (\S+)$")


def _clean_name(name: str) -> str:
    """A Vitosoft UI label made fit for an ESPHome entity name: surrounding and
    repeated whitespace removed (several labels end in a space), and "/"
    replaced by U+2044 FRACTION SLASH -- the look-alike ESPHome itself
    substitutes, since "/" is reserved as a URL path separator."""
    return re.sub(r"\s+", " ", (name or "").strip()).replace("/", "⁄")


def _make_names_unique(buckets: dict) -> int:
    """Clean every name (_clean_name), then give every entity whose name collides
    within its platform a qualifier.

    Vitosoft UI names are not unique (one "Pegel" per RF participant, "nicht
    belegt" for every unused slot), and ESPHome refuses a duplicate name per
    platform. ALL members of a colliding group get " (<id>)" appended -- the
    first one too, so none of them reads as the canonical one -- where <id> is
    the entity's own, already unique ``id:`` (the Viessmann technical name).
    Returns the number of renamed entities. Mutates the bucket chunks."""
    renamed = 0
    for chunks in buckets.values():
        seen: dict[str, int] = {}
        found = []
        for _key, _label, lines in chunks:
            name_idx = next((i for i, ln in enumerate(lines) if _NAME_LINE.match(ln)), None)
            if name_idx is None:
                continue
            m = _NAME_LINE.match(lines[name_idx])
            name = _clean_name(m.group(2))
            lines[name_idx] = f'{m.group(1)}"{name}"'
            oid = next((m.group(1) for ln in lines if (m := _ID_LINE.match(ln))), None)
            # Compare the name as ESPHome sees it, i.e. with the YAML escapes undone.
            unescaped = name.replace('\\"', '"').replace("\\\\", "\\")
            slug = _esphome_object_id(unescaped)
            seen[slug] = seen.get(slug, 0) + 1
            found.append((lines, name_idx, slug, oid))
        for lines, name_idx, slug, oid in found:
            if seen[slug] > 1 and oid:
                m = _NAME_LINE.match(lines[name_idx])
                lines[name_idx] = f'{m.group(1)}"{m.group(2)} ({oid})"'
                renamed += 1
    return renamed


def _inject_diagnostic(lines: list[str]) -> list[str]:
    """Add entity_category: diagnostic once, if not already present."""
    if any(ln.strip().startswith("entity_category:") for ln in lines):
        return lines
    for idx, ln in enumerate(lines):
        if ln.startswith("  name:"):
            lines.insert(idx + 1, "  entity_category: diagnostic")
            return lines
    return lines


def _share_codes_blocks(chunk: list[str], anchors: dict) -> list[str]:
    """Write a ``codes:`` map that already appeared in this catalog as a YAML
    alias of its first occurrence.

    A catalog repeats one map on many slots -- the system fault-history slots
    carry the device's codes each, and with --gfa-chip the GFA slots share the
    chip's map. Done on the chunks in OUTPUT order, after sorting: the first
    occurrence gets ``&<id>_codes`` and every later identical block becomes
    ``*<id>_codes``, so an alias can never precede its anchor (a YAML error).
    Flash is not affected: the component emits identical tables once
    (emit_option_table). ``anchors`` is kept by the caller per catalog."""
    head = next((i for i, ln in enumerate(chunk) if ln.strip() == "codes:"), None)
    if head is None:
        return chunk
    indent = chunk[head][: len(chunk[head]) - len("codes:")]
    end = head + 1
    while end < len(chunk) and chunk[end].startswith(indent + "  0x"):
        end += 1
    body = tuple(chunk[head + 1 : end])
    if body in anchors:
        return [*chunk[:head], f"{indent}codes: *{anchors[body]}  # same map as above", *chunk[end:]]
    oid = next((ln.split("id:", 1)[1].strip() for ln in chunk if ln.strip().startswith("id:")), "map")
    anchors[body] = f"{oid}_codes"
    return [*chunk[:head], f"{indent}codes: &{anchors[body]}", *chunk[head + 1 :]]


def generate(
    catalog: Catalog,
    device: str,
    profile: str,
    include_re: str | None,
    exclude_re: str | None,
    emit_device_id: bool = True,
    emit_error_history: bool = True,
    error_codes: bool = True,
    error_code_set: str = "auto",
    reachable_only: bool = True,
    order: str = "address",
    protocol: str | None = None,
    stats: dict | None = None,
    partial_writes: bool = True,
    gfa_chip: str | None = None,
) -> str:
    """Render the package for one device token.

    ``order`` is "address" (entities sorted by Optolink address) or "group"
    (entities sorted by the Vitosoft navigation tree -- ecnEventTypeGroup --
    with a section comment per functional group). ``stats``, if a dict is
    passed, receives {"entities", "real_entities", "comments", "gfa_ids"} so
    callers (export_all) can tell a real catalog from an empty shell without
    parsing the output."""
    # Which Optolink protocol this catalog targets. It changes what is
    # reachable, whether anything is writable, and whether `access:` is
    # emitted -- see _is_reachable / _is_writable / FCREAD_TO_GWG_ACCESS.
    protocol = _protocol_for_device(device, protocol)

    events = catalog.events_for(device)
    if not events:
        raise SystemExit(
            f"device {device!r} not found or has no events. Run with --list-devices to see available device tokens."
        )

    lang = getattr(catalog, "culture", "de")  # the generator's own names (_UI_TEXT)
    codes_map, codes_note = _resolve_fault_codes(catalog, device, error_codes, error_code_set)
    gfa_map, gfa_note = _resolve_gfa_codes(catalog, gfa_chip) if error_codes else (None, "")

    inc = re.compile(include_re) if include_re else None
    exc = re.compile(exclude_re) if exclude_re else None

    # Error-history events, emitted in a second pass. A unit whose every real
    # datapoint needs KBUS/GFA/RPC access (Dekamatik cascades, M-Bus meters,
    # ...) -- or whose only reachable rows become comments (a bare status enum
    # with no options, a >4-byte blob) -- is not an Optolink device, and the
    # generic 0x7507 system fault history would poll an address that can never
    # answer. That is decided after the main pass, on the true count of emitted
    # entities: the status enum at 0x7561 passes every cheap "reachable
    # datapoint" check yet emits nothing. FehlerHis* per-slot archives carry
    # their own FCRead and are gated only on that, since they are genuine
    # per-unit datapoints.
    deferred_history: list[Event] = []

    # Per-entity chunks: (sort_key, group_label, lines). Emitted per platform in
    # key order; in "group" order a section comment is inserted whenever the
    # group label changes. In "address" order the keys are the processing
    # sequence (events are pre-sorted by address).
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

    # De-duplication identity: the FIELD -- (address, byte_position,
    # bit_position) -- not the base address alone, since one block carries many
    # distinct fields at different BytePositions (HWIndex_SM1/SWIndex_SM1 at
    # 0x0A68, the RF_TeilnehmerInfo_* families, ...). True duplicates (the same
    # field via several event variants) still fold into one entity. The access
    # mode is part of the identity under GWG: its address space is per mode, so
    # 0x01/port and 0x01/eeprom are different datapoints. It is empty for every
    # other protocol but an RPC reading, whose parameter is part of its identity
    # too: the sixteen LON participant entries share 0xA010 and differ only in it.
    def _field_key(ev: Event) -> tuple:
        access = _access_for(ev, protocol)
        prefix = tuple(_rpc_prefix(ev) or ()) if access == "rpc" else ()
        return (ev.address, ev.byte_position or 0, ev.bit_position or 0, access, prefix)

    seen_addr: set[tuple] = set()
    used_ids: set[str] = set()
    gfa_ids: list[str] = []  # ids of the GFA burner-control slots (gfa_companion)
    kept = 0
    real_entities = 0  # emitted entities that are NOT the hub-fed device_id
    dropped_unreachable = 0
    dropped_lon_inputs = 0  # nvi* left out by the profile (_is_lon_input)
    rpc_readings = 0  # emitted with `access: rpc` (P300 remote procedure readings)
    dropped_rpc_readings = 0  # left out by the profile (_profile_keep)
    dropped_wide_addr = 0
    history_suppressed = False
    gwg_system_history_suppressed = False

    # Hub-fed device identity (covers the 0xF8..0xFB identification reads,
    # which are then suppressed below).
    if emit_device_id:
        oid = _make_obj_id("device_type", used_ids)
        _add_chunk("text_sensor", None, _device_id_lines(oid, lang))
        kept += 1

    ro_blocks = _read_only_blocks(events, protocol)

    for ev in sorted(events, key=lambda e: e.address or 0):
        # Special datapoints are handled independently of profile/name filters.
        if emit_device_id and _is_identification(ev):
            continue  # represented by the device_id entity above

        if emit_error_history and _is_error_history(ev):
            # The SYSTEM history (ecnsysEventType~Error / the 0x7507 fallback)
            # and the GFA (Feuerungsautomat) FehlerHis* archive are different
            # subsystems and BOTH emit -- the system one owns "Letzter Fehler"
            # and the fault-code map; GFA slots are named, disabled, and carry
            # no Vitotronic map. A FehlerHis* slot is checked on its own FCRead
            # like any datapoint; the generic system archive additionally
            # needs the device to emit at least one real entity (second pass).
            # An archive read by procedure call is one call per slot index (an
            # array, which _is_rpc_reading refuses); the slot lines carry no
            # prefix, so such an archive stays out even with the filter off.
            if (reachable_only and not _is_reachable(ev, protocol)) or _access_for(ev, protocol) == "rpc":
                dropped_unreachable += 1
                continue
            deferred_history.append(ev)
            continue

        # GWG addresses a single byte. The guard sits above every remaining
        # branch, so the fan-out paths (Schaltzeiten below) are covered too; a
        # wider address would make the hub reject the whole catalog at config
        # time. Error history is deferred above, and the second pass applies the
        # same guard per slot.
        if protocol == PROTO_GWG and ev.address is not None and ev.address > 0xFF:
            dropped_wide_addr += 1
            continue

        # Weekday switching-time programs: one 56-byte token -> 7 per-day `text`
        # entities at base + day*8 (only the 8-byte-record shape; see
        # _is_schaltzeiten).
        if _is_schaltzeiten(ev):
            if (reachable_only and not _is_reachable(ev, protocol)) or _access_for(ev, protocol) == "rpc":
                dropped_unreachable += 1
                continue
            # The block's BASE passed the 8-bit guard above, but the fan-out
            # reaches base + 6*8, so a base near 0xFF would still emit
            # out-of-range weekday addresses. Drop the whole block rather than
            # a partial week.
            if protocol == PROTO_GWG and ev.address is not None:
                last = ev.address + (len(_WEEKDAYS) - 1) * _SCHALTZEITEN_RECORD_LEN
                if last > 0xFF:
                    dropped_wide_addr += 1
                    continue
            if ev.address is not None and _field_key(ev) in seen_addr:
                continue
            if ev.address is not None:
                seen_addr.add(_field_key(ev))
            sz_access = _access_for(ev, protocol)
            for entry in _schaltzeiten_entries(ev, lang):
                oid = _make_obj_id(entry["seed"], used_ids)
                _add_chunk("text", ev, _schaltzeiten_lines(entry, oid, sz_access))
                kept += 1
                real_entities += 1
            continue

        if not _profile_keep(ev, profile):
            if _is_lon_input(ev) and (not reachable_only or _is_reachable(ev, protocol)):
                dropped_lon_inputs += 1
            elif _is_lon_bound_rpc(ev) and protocol == PROTO_P300:
                dropped_rpc_readings += 1
            continue
        target = ev.name or ""
        if inc and not inc.search(target):
            continue
        if exc and exc.search(target):
            continue
        if ev.address is not None and _field_key(ev) in seen_addr:
            continue

        if reachable_only and not _is_reachable(ev, protocol):
            dropped_unreachable += 1
            continue

        spec = build_entity_spec(ev, profile, protocol, partial_writes, ro_blocks)
        if spec is None:
            continue
        if isinstance(spec, CommentSpec):
            comments.extend(spec.lines)
            continue
        # An RPC reading needs a platform that takes `access: rpc`, which only the
        # read-only ones do; anything else could not carry its prefix.
        if _access_for(ev, protocol) == "rpc" and spec.platform not in RPC_PLATFORMS:
            dropped_unreachable += 1
            continue

        if ev.address is not None:
            seen_addr.add(_field_key(ev))

        # Writable operating-mode select: prepend the hardware caveat.
        if spec.platform == "select" and _is_mode_select(ev):
            spec.leading_comments = _mode_select_caveat() + spec.leading_comments
        # Fault / status / health registers -> Diagnostics category.
        if spec.platform in ("sensor", "binary_sensor", "text_sensor") and _is_diagnostic(ev) and not spec.entity_category:
            spec.diagnostic_after_name = True

        # The stable technical identifier as id: (the join key back to Viessmann).
        spec.id = _make_obj_id(ev.tech or ev.name, used_ids)

        # GWG: the access mode is half the datapoint's identity (see
        # FCREAD_TO_GWG_ACCESS), so it travels with the address. Emitted only
        # for the platforms whose schema accepts it. A writable datapoint names
        # both function codes: one mode drives both directions.
        access = _access_for(ev, protocol)
        if access == "rpc":
            # P300 remote procedure call reading: the access line and the call's
            # parameter travel with the address, like a GWG mode does.
            spec.access = access
            spec.access_source = ev.fc_read
            spec.prefix = _rpc_prefix(ev)
            spec.prefix_source = ev.prefix_read
            rpc_readings += 1
        elif access and spec.platform in ACCESS_PLATFORMS:
            spec.access = access
            spec.access_source = f"{ev.fc_read} / {ev.fc_write}" if _is_writable(ev, protocol) else ev.fc_read

        _add_chunk(spec.platform, ev, render_entity_lines(spec))
        kept += 1
        real_entities += 1

    # Second pass: system and GFA fault history. The count of emitted real
    # entities is now known, so the generic system archive can be gated on it
    # (a device that emitted nothing real is not on Optolink).
    for ev in deferred_history:
        if real_entities == 0 and _error_history_slot(ev) is None:
            history_suppressed = True
            continue
        # The GENERIC system archive (ecnsysEventType~Error, slot None) is a
        # Vitotronic construct with fixed 16-bit slot addresses 0x7507 ..
        # 0x7558. Vitosoft links it to GWG device tokens too, but GWG addresses
        # a single byte, so none of its slots is expressible there. Per-unit
        # FehlerHis* archives (slot not None) are real device datapoints and go
        # through the address guard below.
        if protocol == PROTO_GWG and _error_history_slot(ev) is None:
            history_suppressed = True
            gwg_system_history_suppressed = True
            continue
        # Error-history slots are ordinary reads, so under GWG they carry their
        # access mode like anything else.
        access = _access_for(ev, protocol)
        for entry in _error_history_entries(ev, lang):
            if entry["address"] is None:
                continue
            # Same 8-bit guard as the main pass.
            if protocol == PROTO_GWG and entry["address"] > 0xFF:
                dropped_wide_addr += 1
                continue
            # Same field identity as the main pass (_field_key), so a slot
            # whose address an ordinary datapoint already claimed is skipped.
            key = (entry["address"], 0, 0, access, ())
            if key in seen_addr:
                continue
            seen_addr.add(key)
            oid = _make_obj_id(entry["seed"], used_ids)
            if entry.get("gfa"):
                gfa_ids.append(oid)
            lines = _error_history_lines(
                entry,
                oid,
                codes_map,
                codes_note,
                gfa_map,
                gfa_note,
                access=access,
                fc_read=ev.fc_read,
            )
            _add_chunk("text_sensor", ev, lines)
            kept += 1
            real_entities += 1

    renamed = _make_names_unique(buckets)

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
        if protocol == PROTO_GWG:
            out.append(f"# FCRead filter ON (GWG): {dropped_unreachable} datapoint(s) whose access")
            out.append("# method is not one this engine can select via `access:` (KBUS_VIRTUAL_READ")
            out.append("# and friends) were omitted. Re-run with --no-reachable-only to include them.")
        else:
            out.append(f"# FCRead filter ON: {dropped_unreachable} datapoint(s) requiring a non-Optolink")
            out.append("# access method (GFA_READ/RPC/PROZESS/KBUS/OT) were omitted as unreachable")
            out.append("# via VitoWiFi's read. Re-run with --no-reachable-only to include them.")
        out.append("#")
    if gwg_system_history_suppressed:
        out.append("# The generic system fault archive (0x7507..0x7558) is NOT emitted: those")
        out.append("# are fixed 16-bit Vitotronic addresses and GWG addresses a single byte,")
        out.append("# so no slot of it is reachable on this protocol.")
        out.append("#")
    if protocol == PROTO_GWG:
        out.append("# GWG catalog. Every entity carries `access:` -- the Vitosoft FCRead that")
        out.append("# selects the read TYPE byte. GWG's address space is PER ACCESS MODE, so")
        out.append("# address alone is NOT a datapoint identity here: dropping or changing an")
        out.append("# `access:` line silently points the entity at a different register.")
        out.append("# Writable datapoints are emitted as number/select/switch and write in the")
        out.append("# SAME `access:` mode they read in (Vitosoft pairs GWG reads and writes")
        out.append("# that way, e.g. EEPROM_READ with EEPROM_WRITE). Nothing on a GWG unit is")
        out.append("# writable in `physical` mode; the writable ones are `eeprom` and `be`.")
        out.append("# The KMBUS modes have no write TYPE byte at all and stay read-only.")
        out.append("#")
        out.append("# WRITES ARE UNCONFIRMED ON HARDWARE. The write TYPE bytes come from the")
        out.append("# openv wiki table and are corroborated by vogod, but no GWG write has")
        out.append("# been verified end-to-end anywhere, and the placement of the 0x04")
        out.append("# terminator in a write frame has no source at all (see")
        out.append("# gwgWriteEotBeforePayload). Reads are unaffected by that choice.")
        out.append("#")
        out.append("# On the read side only `physical` has hardware confirmation; the other")
        out.append("# modes are sourced from Vitosoft, the openv wiki and vcontrold, which")
        out.append("# agree. Verify on the unit before relying on a value.")
        if dropped_wide_addr:
            out.append("#")
            out.append(f"# {dropped_wide_addr} datapoint(s) with an address above 0xFF were dropped")
            out.append("# (GWG addresses a single byte).")
        out.append("#")
    if rpc_readings:
        noun = "entity" if rpc_readings == 1 else "entities"
        out.append(f"# Remote procedure readings: {rpc_readings} {noun} read by P300 function code")
        out.append("# 0x07 (`access: rpc`), with the catalog's parameter (`prefix:`) where it has one.")
        out.append("# Other remote procedure calls are procedures with an effect (resets,")
        out.append("# clearing a history) and are never polled.")
        out.append("#")
    if dropped_rpc_readings:
        out.append(f"# Remote procedure readings omitted: {dropped_rpc_readings} (the LON participant")
        out.append("# list and LON identity, read by P300 function code 0x07). They answer only")
        out.append("# with a LON module. --profile full includes them.")
        out.append("#")
    if dropped_lon_inputs:
        out.append(f"# LON inputs omitted: {dropped_lon_inputs} network-variable input(s) (nvi*). A LON")
        out.append("# master writes them; without a LON module they hold no data (0xFF")
        out.append("# 'invalid'). --profile full includes them.")
        out.append("#")
    if history_suppressed:
        out.append("# System error history omitted: this unit has NO Optolink-reachable")
        out.append("# datapoints (KBUS/GFA/RPC-only device), so the generic 0x7507 fault")
        out.append("# archive can never answer. Re-run with --no-reachable-only to force it.")
        out.append("#")
    if renamed:
        out.append(f"# {renamed} entity name(s) repeat within their platform in Vitosoft (e.g. one")
        out.append("# 'Pegel' per RF participant); each such name carries its id in parentheses,")
        out.append("# since ESPHome requires a unique name per platform.")
        out.append("#")
    out.append("# VERIFY ON HARDWARE: not every address answers on every firmware.")
    out.append("# Run `esphome config` then `esphome compile`/`run` before relying")
    out.append("# on any value. number entities whose borders were absent in the")
    out.append("# export get min/max/step = 0/0/1 placeholders: valid config, but")
    out.append("# pinned to 0 until you fill in the real bounds.")
    out.append("# ============================================================")
    out.append("")

    code_anchors: dict = {}  # codes block -> its YAML anchor (_share_codes_blocks)
    for platform in ("sensor", "binary_sensor", "number", "select", "switch", "text", "text_sensor"):
        if buckets[platform]:
            out.append(f"{platform}:")
            last_label = None
            first = True
            for _key, label, chunk in sorted(buckets[platform], key=lambda c: c[0]):
                # One blank line between entities (not before the first in a
                # section, and before the group header rather than after it, so
                # a header stays attached to the block it introduces). YAML
                # ignores the blank lines; they keep long catalogs readable.
                if not first:
                    out.append("")
                first = False
                if order == "group" and label and label != last_label:
                    out.append(f"  # --- {label} ---")
                last_label = label
                out.extend(_share_codes_blocks(chunk, code_anchors))
            out.append("")

    if comments:
        out.append("# --- datapoints needing custom decode (not auto-emitted) ---")
        out.extend(comments)
        out.append("")

    if stats is not None:
        stats["entities"] = kept
        stats["real_entities"] = real_entities  # emitted entities excluding the hub-fed device_id
        stats["comments"] = len(comments)
        stats["gfa_ids"] = gfa_ids

    return "\n".join(out)
