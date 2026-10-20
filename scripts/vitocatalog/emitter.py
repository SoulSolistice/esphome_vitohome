"""Unified datapoint analyzer and EntitySpec builder (bitfields + byte scalars)."""

from __future__ import annotations

from .constants import (
    _CONV_LENGTHS,
    _CONV_SCALE,
    _FACTOR_PRESETS,
    COMMAND_STATE_ADDR,
    CONVERSION_MAP,
    COUNTER,
    DIV,
    MAX_P300_READ_LENGTH,
    MAX_TEXT_BLOCK_LENGTH,
    NON_NUMERIC_CONVERSIONS,
    PLAIN,
    PROTO_P300,
)
from .models import CommentSpec, EntitySpec, Event
from .special import _yaml_str
from .transforms import (
    _bit_field,
    _block_write_blocker,
    _bound_fits,
    _declared_unsigned,
    _dedup_option_labels,
    _device_class_for,
    _enum_bool_pair,
    _enum_options,
    _enum_read_length,
    _is_writable,
    _llround,
    _negative_option_count,
    _poll_for,
    _scale_from_bounds,
    _unit_for,
    _unit_negated,
)


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


def render_entity_lines(spec: EntitySpec) -> list[str]:
    """Serialize an EntitySpec into its YAML lines (without section indentation).

    The line order is fixed: comments and NOTEs, platform/type/name, id, the
    diagnostic category, address (+ access), the read shape (length,
    byte_offset, byte_length), state_address (+ access), bit_mask, converter,
    signed, unit/device_class/icon, platform-specific keys, polling, options,
    codes."""
    lines: list[str] = list(spec.leading_comments)
    lines.extend(f"  # NOTE: {n}" for n in spec.notes)

    lines.append("- platform: vitohome")
    if spec.platform_type:
        lines.append(f"  type: {spec.platform_type}")
    lines.append(f"  name: {_yaml_str(spec.name)}")
    # id directly after name, then the diagnostic category: the committed
    # catalogs use this order.
    if spec.id:
        lines.append(f"  id: {spec.id}")
    if spec.diagnostic_after_name:
        lines.append("  entity_category: diagnostic")

    # GWG: the access mode is half the datapoint's identity (see
    # FCREAD_TO_GWG_ACCESS), so it travels with the address it applies to --
    # the read address, which is state_address when the entity has one.
    if spec.address is not None:
        lines.append(f"  address: 0x{spec.address:04X}")
        if spec.access and spec.state_address is None:
            if spec.access_source:
                lines.append(f"  access: {spec.access}  # Vitosoft {spec.access_source}")
            else:
                lines.append(f"  access: {spec.access}")
        # `access: rpc`: the call's parameter travels with it. An RPC reading is
        # read-only, so it never has a state_address.
        if spec.prefix:
            prefix = ", ".join(f"0x{b:02X}" for b in spec.prefix)
            comment = f"  # Vitosoft PrefixRead {spec.prefix_source}" if spec.prefix_source else ""
            lines.append(f"  prefix: [{prefix}]{comment}")

    if spec.length is not None:
        lines.append(f"  length: {spec.length}")
    if spec.byte_offset is not None:
        lines.append(f"  byte_offset: {spec.byte_offset}")
    if spec.byte_length is not None:
        lines.append(f"  byte_length: {spec.byte_length}")

    if spec.state_address is not None:
        lines.append(
            f"  state_address: 0x{spec.state_address:04X}  # live state read here; address above is the write/command target"
        )
        if spec.access:
            if spec.access_source:
                lines.append(f"  access: {spec.access}  # Vitosoft {spec.access_source}")
            else:
                lines.append(f"  access: {spec.access}")

    if spec.bit_mask is not None:
        if spec.bit_mask_comment:
            lines.append(f"  bit_mask: 0x{spec.bit_mask:02X}  # {spec.bit_mask_comment}")
        else:
            lines.append(f"  bit_mask: 0x{spec.bit_mask:02X}")

    if spec.converter:
        lines.append(f"  converter: {spec.converter}")

    if spec.signed is not None:
        s_val = "true" if spec.signed else "false"
        if spec.signed_comment:
            lines.append(f"  signed: {s_val}  # {spec.signed_comment}")
        else:
            lines.append(f"  signed: {s_val}")

    if spec.unit:
        lines.append(f"  unit_of_measurement: {_yaml_str(spec.unit)}")
    if spec.device_class:
        lines.append(f"  device_class: {spec.device_class}")
    if spec.icon:
        lines.append(f"  icon: {_yaml_str(spec.icon)}")
    if spec.entity_category and not spec.diagnostic_after_name:
        lines.append(f"  entity_category: {spec.entity_category}")

    # Switch values: shown only when they are not the 1/0 default. Bit fields
    # (option_hex_width None) use decimal field values, byte fields hex at the
    # field's width.
    if spec.platform == "switch" and spec.on_value is not None and spec.off_value is not None:
        if (spec.on_value, spec.off_value) != (1, 0):
            if spec.option_hex_width is None:
                lines.append(f"  on_value: {spec.on_value}  # {spec.on_label}")
                lines.append(f"  off_value: {spec.off_value}  # {spec.off_label}")
            else:
                w = 2 * spec.option_hex_width
                lines.append(f"  on_value: 0x{spec.on_value:0{w}X}  # {spec.on_label}")
                lines.append(f"  off_value: 0x{spec.off_value:0{w}X}  # {spec.off_label}")
        elif spec.option_hex_width is None:
            lines.append(f"  # on {spec.on_value} = {spec.on_label}; off {spec.off_value} = {spec.off_label} (field values)")
        else:
            lines.append(f"  # on 0x{spec.on_value:02X} = {spec.on_label}; off 0x{spec.off_value:02X} = {spec.off_label}")

    if spec.platform == "number":
        if spec.bounds_placeholder_comment:
            lines.append("  # Borders absent, or out of range for this byte width, in")
            lines.append("  # the export. These placeholders pass `esphome config` but")
            lines.append("  # pin the value to 0 - set real min/max/step before use.")
        lines.append(f"  min_value: {spec.min_value}")
        lines.append(f"  max_value: {spec.max_value}")
        lines.append(f"  step: {spec.step}")
        if spec.mode:
            lines.append(f"  mode: {spec.mode}")

    if spec.platform == "sensor":
        if spec.negated_unit_comment:
            lines.append("  # stored as a magnitude (Vitosoft unit '-dBm'); negated to dBm")
        if spec.filters:
            # The true value transform, applied by ESPHome after the raw decode.
            lines.append("  filters:")
            for f_name, f_arg in spec.filters:
                lines.append(f"    - {f_name}: {f_arg:.12g}")
        if spec.state_class:
            lines.append(f"  state_class: {spec.state_class}")
        if spec.accuracy_decimals is not None:
            lines.append(f"  accuracy_decimals: {spec.accuracy_decimals}")

    if spec.disabled_by_default:
        lines.append("  disabled_by_default: true")
    if spec.update_interval is not None:
        lines.append(f"  update_interval: {spec.update_interval}s")

    if spec.options:
        lines.append("  options:")
        if spec.option_hex_width is None:
            for v, label in spec.options:
                lines.append(f"    {v}: {_yaml_str(label)}")
        else:
            w = 2 * spec.option_hex_width
            for v, label in spec.options:
                lines.append(f"    0x{v:0{w}X}: {_yaml_str(label)}")

    if spec.codes is not None:
        lines.append(f"  # {spec.codes_note or 'Fault-code map:'}")
        lines.append("  codes:")
        for code, text in sorted(spec.codes.items()):
            lines.append(f"    0x{code:02X}: {_yaml_str(text)}")

    if spec.trailing_comments:
        lines.extend(spec.trailing_comments)

    return lines


def _string_spec(
    name: str,
    kind: str,
    addr: int,
    field_len: int,
    field_off: int,
    block_len: int,
) -> EntitySpec | None:
    """An ascii or utf16 text_sensor, or None if the block read would exceed
    MAX_TEXT_BLOCK_LENGTH (the caller then emits a comment).

    A string field at BytePosition > 0 is read as an ALIGNED BLOCK at the block
    base plus byte_offset. Adding the offset to the address instead names a
    datapoint that does not exist: Beschriftung_HK1~0x7360 (BlockLength 42,
    BytePosition 2) would become 0x7362, which P300 answers with an error
    telegram at any read width and KW with 0xFF fill.
    """
    if not field_off:
        return EntitySpec(
            platform="text_sensor",
            platform_type=kind,
            name=name,
            address=addr,
            length=field_len,
            entity_category="diagnostic" if kind == "ascii" else None,
        )
    read_len = max(block_len, field_off + field_len)
    if read_len > MAX_TEXT_BLOCK_LENGTH:
        return None
    trailing: list[str] = []
    if kind == "utf16":
        trailing = [
            f"  # aligned block read: {field_off + field_len} bytes at the block base. The widest read",
            "  # proven on P300 hardware is 22 bytes; verify before enabling.",
        ]
    return EntitySpec(
        platform="text_sensor",
        platform_type=kind,
        name=name,
        address=addr,
        length=read_len,
        byte_offset=field_off,
        byte_length=field_len,
        entity_category="diagnostic" if kind == "ascii" else None,
        disabled_by_default=(kind == "utf16"),
        trailing_comments=trailing,
    )


def _string_lines(
    name: str,
    kind: str,
    addr: int,
    field_len: int,
    field_off: int,
    block_len: int,
) -> list[str] | None:
    """Back-compat helper returning raw lines for ascii/utf16 string fields:
    the type, name and read shape of _string_spec only, without
    entity_category, disabled_by_default or the utf16 caveat."""
    spec = _string_spec(name, kind, addr, field_len, field_off, block_len)
    if spec is None:
        return None
    lines = [
        "- platform: vitohome",
        f"  type: {kind}",
        f"  name: {_yaml_str(name)}",
        f"  address: 0x{addr:04X}",
        f"  length: {spec.length}",
    ]
    if field_off:
        lines.append(f"  byte_offset: {field_off}")
        lines.append(f"  byte_length: {field_len}")
    return lines


def _build_bit_field_spec(
    ev: Event,
    protocol: str,
    partial_writes: bool = True,
    ro_blocks: frozenset = frozenset(),
) -> EntitySpec | CommentSpec:
    """A bit field (BitLength 1..8) as a native ``bit_mask`` entity.

    Every bit field except a READ-ONLY single bit comes here (those take the
    binary_sensor path of build_entity_spec). With ``bit_mask`` the component
    reads the byte first and writes back only the field (vitohome.h,
    rmw_pending_), so a write leaves the other bits of the byte -- often
    unrelated coding flags -- untouched.

    In a 1-byte datapoint (every GWG bit field) the write is a read-modify-write
    of that byte. In a wider block (the P300 Neptun_* outputs, bit 0x01 of
    2-byte blocks) it is a read-modify-write of the WHOLE block -- the device
    writes its whole datapoint -- when _block_write_blocker() allows it; else
    read-only with the reason. Fields whose scale is inferred
    (_scale_from_bounds) or carries an offset (MultOffset with ConversionOffset
    != 0) stay read-only: there is no encode path for them.
    """
    name = ev.name
    addr = ev.address
    bit_len = ev.bit_length or 0
    bit_pos = ev.bit_position or 0
    bf = _bit_field(ev)
    if bf is None:
        return CommentSpec(
            [
                f"# {name} @ 0x{addr:04X}: {bit_len}-bit field at BitPosition {bit_pos} does not fit in one byte -> custom decode"
            ]
        )
    byte, shift, fmax = bf
    mask = fmax << shift
    block_len = ev.block_length or ev.byte_length or 1
    if ev.byte_position and ev.byte_position != byte:
        return CommentSpec(
            [
                (
                    f"# {name} @ 0x{addr:04X}: BitPosition {bit_pos} implies byte {byte} but the "
                    f"export declares BytePosition {ev.byte_position} -- contradictory, needs hardware "
                    "confirmation before it can be emitted"
                )
            ]
        )
    if block_len > MAX_P300_READ_LENGTH or byte >= block_len:
        return CommentSpec(
            [
                (
                    f"# {name} @ 0x{addr:04X}: {bit_len}-bit field in byte {byte} of a {block_len}-byte "
                    "block exceeds the single-telegram read -> custom handling"
                )
            ]
        )
    mask_comment = f"Vitosoft BitPosition {bit_pos}, BitLength {bit_len} (MSB-first)"
    notes: list[str] = []

    writable = _is_writable(ev, protocol)
    block_mode = False
    if writable and not partial_writes:
        writable = False
        notes.append("partial writes disabled (--no-partial-writes) -> read-only")
    if writable and block_len > 1:
        why = _block_write_blocker(ev, protocol, block_len, 1, ro_blocks, partial_writes)
        if why:
            writable = False
            notes.append(f"field in byte {byte} of a {block_len}-byte block: {why} -> read-only")
        else:
            block_mode = True
            notes.append(f"writes are a read-modify-write of the whole {block_len}-byte block")
    if writable and COMMAND_STATE_ADDR.get(addr) is not None:
        writable = False
        notes.append("command register with a separate state address: no read-modify-write -> read-only")

    # Block mode: the whole block at the base, read AND written there, with the
    # field's byte_offset -- even 0, which is what selects block mode.
    # Otherwise byte 0 is a 1-byte read at the block base (aligned on P300), and
    # a deeper byte the whole block at the base plus byte_offset.
    if block_mode:
        read_len, read_off = block_len, byte
    elif byte == 0:
        read_len, read_off = 1, None
    else:
        read_len, read_off = block_len, byte

    _conv0, conv_kind = CONVERSION_MAP.get(ev.conversion, (None, PLAIN))
    poll = _poll_for(ev, conv_kind, protocol)

    # --- enum fields ---------------------------------------------------------
    enum_opts = _dedup_option_labels(_enum_options(ev))
    if enum_opts:
        # An option the field cannot hold means the export contradicts itself:
        # its value list describes a wider value than its BitLength. E.g.
        # SM1_Sensor_7_Status_GWG is BitLength 7 (mask 0x7F) but lists 128 =
        # "Nicht verwendet"; the 0x80 bit is the sibling
        # SM1_Sensor_7_Status_Verwendet flag, and dropping 128 and masking would
        # report "Unterbrechung" (open circuit) for a sensor that is not fitted.
        # Neither reading is safe to guess, so -- as for a BitPosition that
        # contradicts BytePosition (docs/design_notes.md SS8f) -- it becomes a
        # comment.
        over = sorted({v for v, _ in enum_opts if v > fmax})
        if over:
            return CommentSpec(
                [
                    (
                        f"# {name} @ 0x{addr:04X}: option value(s) {', '.join(str(v) for v in over)} do not fit the "
                        f"{bit_len}-bit field (0..{fmax}) -- the export's value list and BitLength disagree; "
                        "not emitted"
                    )
                ]
            )
        fit = [(v, label) for v, label in enum_opts if v >= 0]
        neg = _negative_option_count(ev)
        if neg:
            notes.append(f"{neg} negative enum option value(s) from the export omitted")
        if not fit:
            return CommentSpec([f"# {name} @ 0x{addr:04X}: no enum option fits the {bit_len}-bit field"])
        if writable:
            pair = _enum_bool_pair(ev, fit)
            if pair:
                on_v, off_v, on_l, off_l = pair
                return EntitySpec(
                    platform="switch",
                    name=name,
                    address=addr,
                    length=read_len,
                    byte_offset=read_off,
                    bit_mask=mask,
                    bit_mask_comment=mask_comment,
                    on_value=on_v,
                    off_value=off_v,
                    on_label=on_l,
                    off_label=off_l,
                    option_hex_width=None,
                    disabled_by_default=True,
                    update_interval=poll,
                    notes=notes,
                )
            return EntitySpec(
                platform="select",
                name=name,
                address=addr,
                length=read_len,
                byte_offset=read_off,
                bit_mask=mask,
                bit_mask_comment=mask_comment,
                options=fit,
                option_hex_width=None,
                disabled_by_default=True,
                update_interval=poll,
                notes=notes,
            )
        if bit_len == 1:
            # A read-only flag keeps the binary_sensor shape every other bit uses.
            return EntitySpec(
                platform="binary_sensor",
                name=name,
                address=addr,
                length=block_len,
                byte_offset=byte or None,
                bit_mask=mask,
                bit_mask_comment=mask_comment,
                disabled_by_default=True,
                update_interval=poll,
                notes=notes,
            )
        return EntitySpec(
            platform="text_sensor",
            platform_type="enum",
            name=name,
            address=addr,
            length=read_len,
            byte_offset=read_off,
            bit_mask=mask,
            bit_mask_comment=mask_comment,
            options=fit,
            option_hex_width=None,
            disabled_by_default=True,
            update_interval=poll,
            notes=notes,
        )

    # --- numeric fields ------------------------------------------------------
    filters: list[tuple[str, float]] = []
    conv_factor = getattr(ev, "conv_factor", None)
    conv_offset = getattr(ev, "conv_offset", None)
    eff_factor = conv_factor if conv_factor not in (None, 0.0) else None
    conv = _conv0
    if ev.conversion in ("", "NoConversion"):
        conv = "noconv"
        if eff_factor is not None:
            # The factor/offset columns on NoConversion bit fields are copies of
            # a neighbouring MultOffset row, not this field's encoding; see
            # _scale_from_bounds.
            notes.append(
                f"export factor/offset ({eff_factor:g}, {conv_offset or 0:g}) ignored (stale on NoConversion bit fields)"
            )
        bs = _scale_from_bounds(ev, protocol)
        if bs is not None and bs.kind != "ambiguous":
            filters.append(("multiply", bs.scale))
            if bs.offset:
                filters.append(("offset", bs.offset))
            notes.append(
                f"fractional stepping: borders imply value = field * {bs.scale:g}"
                + (f" + {bs.offset:g}" if bs.offset else "")
                + f" (field {bs.raw_lo}..{bs.raw_hi}). UNVERIFIED inference -- read-only until a hardware "
                "write confirms it"
            )
            writable = False
        elif bs is not None:
            notes.append("fractional stepping with an ambiguous encoding -> raw field value, read-only")
            writable = False
    elif ev.conversion == "MultOffset":
        f_val = eff_factor
        o_val = conv_offset or 0.0
        preset = None
        if f_val is not None and o_val == 0.0:
            for pf, pname in _FACTOR_PRESETS:
                if abs(f_val - pf) < 1e-12:
                    preset = pname
                    break
        if preset:
            conv = preset
            notes.append(f"conversion 'MultOffset' (factor {f_val:g}) -> {preset}")
        else:
            conv = "noconv"
            if f_val is not None and f_val != 1.0:
                filters.append(("multiply", f_val))
            if o_val:
                filters.append(("offset", o_val))
            if filters:
                notes.append(
                    f"conversion 'MultOffset': value = field * {f_val if f_val is not None else 1:g}"
                    + (f" + {o_val:g}" if o_val else "")
                    + " -> filters; no encode path for the transform -> read-only"
                )
                writable = False
    # The field is decoded as one byte: a converter without a 1-byte width
    # becomes noconv plus its scale as a filter, read-only.
    if conv is None or 1 not in _CONV_LENGTHS.get(conv, (1,)):
        scale = _CONV_SCALE.get(conv or "", 1.0)
        if scale != 1.0:
            filters.insert(0, ("multiply", scale))
        notes.append(f"conversion {ev.conversion!r} has no 1-byte converter -> raw field + filter, read-only")
        conv = "noconv"
        writable = False

    unit = _unit_for(ev)
    dc = _device_class_for(unit)
    vals = getattr(ev, "values", None) or []
    if writable:
        scale = _CONV_SCALE.get(conv, 1.0)
        lo = ev.lower or (vals[0].lower if vals else "")
        hi = ev.upper or (vals[0].upper if vals else "")
        step = ev.stepping or (vals[0].stepping if vals else "")

        def _fits(v: str) -> bool:
            try:
                raw = _llround(float(v) / scale)
            except (TypeError, ValueError):
                return False
            return 0 <= raw <= fmax

        if lo != "" and hi != "" and step not in ("", "0") and _fits(lo) and _fits(hi):
            min_v, max_v, step_v = lo, hi, step
        else:
            # The field's own range is known exactly, which beats the 0/0/1
            # placeholder that whole-byte numbers fall back to.
            notes.append(f"borders absent or outside the {bit_len}-bit field -> the field range 0..{fmax}")
            min_v, max_v, step_v = "0", f"{fmax * scale:g}", f"{scale:g}"
        return EntitySpec(
            platform="number",
            name=name,
            address=addr,
            length=read_len,
            byte_offset=read_off,
            bit_mask=mask,
            bit_mask_comment=mask_comment,
            converter=conv,
            unit=unit or None,
            device_class=dc,
            min_value=min_v,
            max_value=max_v,
            step=step_v,
            mode="box",
            disabled_by_default=True,
            update_interval=poll,
            notes=notes,
        )

    return EntitySpec(
        platform="sensor",
        name=name,
        address=addr,
        length=read_len,
        byte_offset=read_off,
        bit_mask=mask,
        bit_mask_comment=mask_comment,
        converter=conv,
        unit=unit or None,
        device_class=dc,
        filters=filters,
        state_class="total_increasing" if conv_kind == COUNTER else "measurement",
        accuracy_decimals=1 if conv_kind == COUNTER else None,
        disabled_by_default=True,
        update_interval=poll,
        notes=notes,
    )


def _emit_bit_field(
    ev: Event,
    protocol: str,
    partial_writes: bool = True,
    ro_blocks: frozenset = frozenset(),
) -> tuple[str, list[str]]:
    """Back-compat wrapper returning (platform, yaml_lines) for a bit field."""
    spec = _build_bit_field_spec(ev, protocol, partial_writes, ro_blocks)
    if isinstance(spec, CommentSpec):
        return ("comment", spec.lines)
    return (spec.platform, render_entity_lines(spec))


def build_entity_spec(
    ev: Event,
    profile: str,
    protocol: str = PROTO_P300,
    partial_writes: bool = True,
    ro_blocks: frozenset = frozenset(),
) -> EntitySpec | CommentSpec | None:
    """Analyze one Event into a structured EntitySpec, CommentSpec, or None.

    ev.name is the resolved friendly name; the stable technical ``id:``, the
    GWG ``access:`` and the diagnostic category are added by generate(), which
    sees the whole package."""
    name = ev.name
    addr = ev.address
    if addr is None:
        return None

    # Non-numeric conversions: surface as a commented hint, never a wrong sensor.
    if ev.conversion in NON_NUMERIC_CONVERSIONS:
        hint = NON_NUMERIC_CONVERSIONS[ev.conversion]
        return CommentSpec([f"# {name} @ 0x{addr:04X}: {ev.conversion} ({hint})"])

    # WRITE-ONLY registers (Vitosoft access type 2: FunktionReset triggers,
    # counter resets, the hydraulic-balance byte blob). There is no state to
    # poll -- reading them returns garbage or NAKs -- and a number/switch would
    # show a meaningless value, so they become a hint instead of a polled
    # entity. The COMMAND_STATE_ADDR command registers carry access type 3 in
    # the export, so this does not shadow them; the guard is defensive.
    if ev.access_type == 2 and COMMAND_STATE_ADDR.get(ev.address) is None:
        return CommentSpec(
            [
                (
                    f"# {name} @ 0x{addr:04X}: WRITE-ONLY register (Vitosoft AccessMode 'Write');"
                    " a trigger with no read-back -> not auto-emitted."
                )
            ]
        )

    # Bit fields: everything except a read-only single bit (binary_sensor below).
    bit_len = ev.bit_length or 0
    if bit_len > 1 or (bit_len == 1 and _is_writable(ev, protocol)):
        return _build_bit_field_spec(ev, protocol, partial_writes, ro_blocks)

    conv, conv_kind = CONVERSION_MAP.get(ev.conversion, (None, PLAIN))
    note: str | None = None
    # Value-transform filters (multiply/offset) for READ-ONLY sensors whose
    # factor cannot be expressed as a component converter. Writables never get
    # filters (there is no encode-side transform); they keep raw noconv + note.
    value_filters: list[tuple[str, float]] = []
    force_signed = False
    writable = _is_writable(ev, protocol)

    # ConversionFactor/ConversionOffset (value = raw * factor + offset).
    # Applied ONLY for MultOffset and for identity conversions carrying a real
    # factor -- never stacked on Div*/Mult*/Sec2Hour presets (the export
    # sometimes echoes the preset's own factor there, e.g. Div2 rows with
    # factor 0.5). Factor -> preset where possible (K90 MultOffset factor 10
    # -> mult10). A factor of exactly 0 is the export's "no scaling" sentinel
    # (the loader already maps it to None; this also covers Events built by
    # other means) -- never a literal 0x multiplier.
    conv_factor = getattr(ev, "conv_factor", None)
    conv_offset = getattr(ev, "conv_offset", None)
    eff_factor = conv_factor if conv_factor not in (None, 0.0) else None
    factor_driven = ev.conversion == "MultOffset" or (ev.conversion in ("", "NoConversion") and eff_factor is not None)

    # Borders + fractional stepping that cannot be a plain raw value win over
    # the factor/offset columns; see _scale_from_bounds. Always read-only:
    # there is no affine encode path, and the inference is unverified on
    # hardware.
    bounds_scale = _scale_from_bounds(ev, protocol)
    if bounds_scale is not None:
        bs = bounds_scale
        factor_driven = False
        conv = "noconv"
        b_lo = bs.raw_lo * bs.scale + bs.offset
        b_hi = bs.raw_hi * bs.scale + bs.offset
        head = (
            f"conversion {ev.conversion or 'none'!r} with fractional stepping: borders {b_lo:g}..{b_hi:g} step {bs.scale:g}"
        )
        stale = f"; export factor/offset ({eff_factor:g}, {conv_offset or 0:g}) ignored" if eff_factor else ""
        if bs.kind == "ambiguous":
            # Raw bytes only: two readings fit and nothing decides between them.
            note = (
                f"{head} fit both signed raw * {bs.scale:g} and raw * {bs.scale:g} + {bs.offset:g}"
                f" (raw {bs.raw_lo}..{bs.raw_hi}){stale}. Encoding unknown -> raw value, read-only"
            )
        else:
            value_filters.append(("multiply", bs.scale))
            if bs.offset:
                value_filters.append(("offset", bs.offset))
            note = (
                f"{head} imply value = raw * {bs.scale:g}"
                + (f" + {bs.offset:g}" if bs.offset else "")
                + f" (raw {bs.raw_lo}..{bs.raw_hi}){stale}. UNVERIFIED inference -- read-only"
                " until a hardware write confirms it"
            )
        if writable:
            writable = False
            note += " (writable in the export; not a number until confirmed)"

    if factor_driven:
        f_val = eff_factor
        o_val = conv_offset or 0.0
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
                # what gets written. The display transform goes into the note.
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
    # whole block at `addr`, ByteLength the field's own width, BytePosition its
    # offset within the block.
    block_len = ev.block_length or ev.byte_length or 1
    length = ev.byte_length or ev.block_length or 1
    field_off = ev.byte_position or 0
    # P300 NAKs an unaligned interior read (KW tolerates it), so a field (1..4
    # bytes) at a non-zero offset is read as an aligned BLOCK at the base with
    # byte_offset (+ byte_length for a multi-byte field): the component fetches
    # the whole block and extracts the field, portable across both protocols.
    # Only within the P300 single-telegram cap.
    field_width = ev.byte_length or block_len
    use_block_extract = field_off > 0 and 1 <= field_width <= 4 and block_len <= MAX_P300_READ_LENGTH
    # The string types carry their own block/offset form (their branch below),
    # so they are exempt from the check here.
    is_string = ev.conversion in ("HexByte2AsciiByte", "HexByte2UTF16Byte")
    if field_off and not use_block_extract and not is_string:
        # INVARIANT: never emit `addr + BytePosition`. That address need not
        # exist: P300 answers an unaligned interior read with an error telegram
        # at any width (0x7362 = Beschriftung_HK1~0x7360 + BytePosition 2), and
        # KW with 0xFF fill, which decodes to a plausible-looking empty value.
        # Every branch below is therefore reached with either field_off == 0
        # (the address IS the field) or an aligned block extract (the address is
        # the block base, paired with byte_offset).
        return CommentSpec(
            [
                (
                    f"# {name} @ 0x{addr:04X}: {field_width}-byte field at BytePosition {field_off} of a "
                    f"{block_len}-byte block -- cannot be expressed as an aligned block read "
                    f"(block > {MAX_P300_READ_LENGTH} bytes, or field > 4 bytes)"
                )
            ]
        )

    enum_opts = _dedup_option_labels(_enum_options(ev))
    neg_opts = _negative_option_count(ev)
    is_bit = bit_len > 0

    # `writable` was resolved above; the demotions below narrow it further.
    # A converter without an encode path (sec2hour, rotatebytes are read-only
    # in the component; number.py accepts only encodable converters) cannot be
    # a `number` -- the config would fail `esphome config`.
    if writable and conv in ("sec2hour", "rotatebytes"):
        writable = False
        note = (note + "; " if note else "") + f"converter {conv!r} is read-only -> demoted to sensor"
    # A display value that needs a multiply/offset filter has no encode-side
    # transform either; read-only keeps the filtered value correct. (Filters
    # are collected only for non-writables above, so this is defensive.)
    if writable and value_filters:
        writable = False
        note = (note + "; " if note else "") + "value transform has no encode path -> demoted to sensor"
    # A writable field smaller than its block -- at BytePosition > 0, or
    # narrower than the block at its base -- cannot be written on its own:
    #   * `addr + BytePosition` is not a declared write address (see the
    #     invariant above), and
    #   * the device writes its WHOLE datapoint, so a narrower write fills the
    #     rest of the block from the telegram (a 1-byte write into
    #     Neptun_ZP~0x7953 stores the telegram's checksum in byte 1).
    # Such a field is written as a whole-block read-modify-write (block mode:
    # read the block at the base, replace the field, write the block back) when
    # _block_write_blocker() allows it; else it is demoted with the reason.
    # (Weekday Schaltzeiten write 8 bytes per day into the 56-byte program, as
    # vcontrold does; they have their own path and never reach this.)
    block_mode = False
    if writable and (field_off > 0 or length < block_len):
        why = _block_write_blocker(ev, protocol, block_len, length, ro_blocks, partial_writes)
        if why:
            writable = False
            note = (note + "; " if note else "") + (
                f"{length}-byte field at BytePosition {field_off} of a {block_len}-byte block: {why} -> demoted to read-only"
            )
        else:
            block_mode = True
            note = (note + "; " if note else "") + f"writes are a read-modify-write of the whole {block_len}-byte block"

    # Byte-array-as-string (HexByte2AsciiByte): the ascii text_sensor type
    # (Sachnummer, Herstellnummer, ...). Read-only device identity; the field
    # can exceed the 4-byte scalar limit (the Herstellnummer is 16 bytes), so
    # this precedes the >4-byte comment fallback below.
    if ev.conversion == "HexByte2AsciiByte":
        if length > 32:
            return CommentSpec([f"# {name} @ 0x{addr:04X}: HexByte2AsciiByte length {length} > 32 -> custom decode"])
        spec = _string_spec(name, "ascii", addr, length, field_off, block_len)
        if spec is None:
            return CommentSpec(
                [
                    (
                        f"# {name} @ 0x{addr:04X}: ascii field of {length} bytes at BytePosition {field_off} "
                        f"of a {block_len}-byte block -- block read exceeds {MAX_TEXT_BLOCK_LENGTH} bytes"
                    )
                ]
            )
        return spec

    # UTF-16LE label (HexByte2UTF16Byte): the editable heating-circuit names
    # Beschriftung_HK1..3 (40 bytes = 20 chars). Emitted read-only (display of
    # the label); editing would need a `text:` platform with a UTF-16 encode
    # path. Precedes the >4-byte comment fallback.
    if ev.conversion == "HexByte2UTF16Byte":
        if length > 40 or (length % 2) != 0:
            return CommentSpec([f"# {name} @ 0x{addr:04X}: HexByte2UTF16Byte length {length} (>40 or odd) -> custom decode"])
        spec = _string_spec(name, "utf16", addr, length, field_off, block_len)
        if spec is None:
            return CommentSpec(
                [
                    (
                        f"# {name} @ 0x{addr:04X}: utf16 field of {length} bytes at BytePosition {field_off} "
                        f"of a {block_len}-byte block -- block read exceeds {MAX_TEXT_BLOCK_LENGTH} bytes"
                    )
                ]
            )
        return spec

    # A field wider than 4 bytes cannot go through the 1..4-byte scalar converters.
    if length > 4 and not is_bit and not enum_opts:
        return CommentSpec([f"# {name} @ 0x{addr:04X}: {ev.conversion or 'raw'} length {length} > 4 bytes -> custom decode"])
    # Scalar reads are clamped to the converter-supported 1..4 bytes.
    num_length = length if length in (1, 2, 3, 4) else max(1, min(4, length))

    # Converter-supported widths (_CONV_LENGTHS): sensor.py/number.py reject an
    # unsupported pair at `esphome config` time. The decode width is the FIELD
    # width, which equals `length` on every scalar path (with or without block
    # extraction); the enum and bit paths never consult the converter.
    # Fallback: the raw bytes as noconv with the true scale as an ESPHome
    # `multiply:` filter, so the reported value stays correct; the signed
    # div2/div10 default survives as an explicit `signed: true`. rotatebytes is
    # big-endian -- a scale filter cannot fix byte order, so an unsupported
    # width there stays a comment.
    if not is_bit and not enum_opts and length not in _CONV_LENGTHS.get(conv, (1, 2, 3, 4)):
        if conv == "rotatebytes":
            return CommentSpec(
                [
                    (
                        f"# {name} @ 0x{addr:04X}: RotateBytes (big-endian) at {length} bytes is not"
                        " decodable by the component (rotatebytes supports 2) -> custom decode"
                    )
                ]
            )
        if writable:
            writable = False
            note = (note + "; " if note else "") + f"converter {conv!r} unsupported at {length} bytes -> demoted to sensor"
        scale = _CONV_SCALE.get(conv, 1.0)
        if scale != 1.0:
            value_filters.append(("multiply", scale))
        force_signed = force_signed or (conv in ("div2", "div10") and not _declared_unsigned(ev))
        note = (note + "; " if note else "") + (
            f"converter {conv!r} unsupported at {length} bytes -> noconv"
            + (f" + multiply filter ({scale:g})" if scale != 1.0 else "")
        )
        conv = "noconv"

    poll = _poll_for(ev, conv_kind, protocol)
    notes: list[str] = []
    if note:
        notes.append(note)
    if neg_opts:
        notes.append(
            f"{neg_opts} negative enum option value(s) from the export omitted"
            " (raw bytes compare unsigned; a negative option can never match)"
        )

    # Write target: in block mode the block base, the whole block and the field
    # (byte_offset, even 0, without state_address is what selects block mode);
    # otherwise the field is the whole datapoint.
    w_len = block_len if block_mode else length
    w_off = field_off if block_mode else None
    w_blen = (length if length != 1 else None) if block_mode else None

    # --- writable: switch, select, or number --------------------------------
    # A writable enum becomes a select when the field is 1-2 bytes AND every
    # option value fits that width: the range components/vitohome/select.py
    # accepts (validate_length_in(1, 2)) and its per-value check
    # (_validate_options), so the select is not rejected at `esphome config`
    # time. Wider or non-fitting enums fall through to the number branch.
    # A semantic on/off pair becomes a switch instead of a two-option select,
    # so HA gets a native toggle. _boolean_pair is deliberately conservative:
    # non-boolean two-option enums (Celsius/Fahrenheit, Einkessel/Mehrkessel)
    # stay selects.
    bool_pair = _enum_bool_pair(ev, enum_opts) if enum_opts else None
    if writable and bool_pair and length in (1, 2) and all(0 <= v < (1 << (8 * length)) for v, _ in enum_opts):
        on_v, off_v, on_l, off_l = bool_pair
        return EntitySpec(
            platform="switch",
            name=name,
            address=addr,
            length=w_len,
            byte_offset=w_off,
            byte_length=w_blen,
            state_address=COMMAND_STATE_ADDR.get(ev.address),
            on_value=on_v,
            off_value=off_v,
            on_label=on_l,
            off_label=off_l,
            option_hex_width=length,
            disabled_by_default=True,
            update_interval=poll,
            notes=notes,
        )

    if writable and enum_opts and length in (1, 2) and all(0 <= v < (1 << (8 * length)) for v, _ in enum_opts):
        return EntitySpec(
            platform="select",
            name=name,
            address=addr,
            length=w_len,
            byte_offset=w_off,
            byte_length=w_blen,
            state_address=COMMAND_STATE_ADDR.get(ev.address),
            options=enum_opts,
            option_hex_width=length,
            disabled_by_default=True,
            update_interval=poll,
            notes=notes,
        )

    vals = getattr(ev, "values", None) or []
    if writable:
        lo = ev.lower or (vals[0].lower if vals else "")
        hi = ev.upper or (vals[0].upper if vals else "")
        step = ev.stepping or (vals[0].stepping if vals else "")
        # A negative lower border means the raw byte(s) are two's-complement.
        # div2/div10 are already signed in the component; every other converter
        # is unsigned by default and needs an explicit `signed: true` to accept
        # a negative bound (otherwise the number validator rejects it and the
        # borders fall back to the 0/0/1 placeholders).
        try:
            _lo_neg = lo != "" and float(lo) < 0
        except (TypeError, ValueError):
            _lo_neg = False
        _default_signed = conv in ("div2", "div10")
        is_signed = _default_signed or _lo_neg
        signed_val: bool | None = None
        signed_comment: str | None = None
        if _lo_neg and not _default_signed:
            signed_val = True
        elif _default_signed and _declared_unsigned(ev):
            # Declared unsigned with a non-negative border: override the
            # converter's signed default (see _declared_unsigned). This also
            # lets an upper border above the signed range fit -- 127.5 on a
            # 1-byte div2 is raw 255, unsigned -- instead of falling back to
            # the 0/0/1 placeholders.
            is_signed = False
            signed_val = False
            signed_comment = f"Vitosoft ParameterType {getattr(ev, 'parameter_type', '')}, borders {lo}..{hi}"
        unit = _unit_for(ev)
        dc = _device_class_for(unit)
        have_bounds = (
            lo != ""
            and hi != ""
            and step not in ("", "0")
            and _bound_fits(lo, length, conv, is_signed)
            and _bound_fits(hi, length, conv, is_signed)
        )
        return EntitySpec(
            platform="number",
            name=name,
            address=addr,
            length=w_len,
            byte_offset=w_off,
            byte_length=w_blen,
            converter=conv,
            signed=signed_val,
            signed_comment=signed_comment,
            unit=unit or None,
            device_class=dc,
            min_value=lo if have_bounds else 0,
            max_value=hi if have_bounds else 0,
            step=step if have_bounds else 1,
            bounds_placeholder_comment=not have_bounds,
            mode="box",
            disabled_by_default=True,
            update_interval=poll,
            notes=notes,
        )

    # --- read-only: single-bit binary_sensor, enum text_sensor, or sensor ---
    if is_bit:
        # BitPosition is the ABSOLUTE bit index within the block; BytePosition
        # is sometimes left 0 in the export, so the byte comes from the bit index.
        bit_pos = ev.bit_position or 0
        byte_off = bit_pos // 8
        # Vitosoft numbers bits MSB-FIRST inside the byte: index 0 is 0x80,
        # index 7 is 0x01. Hardware-confirmed on 0x55DD (VScotHO1_72), which
        # carries GWG_Flamme1 (BitPosition 2) and GWG_Brenner_2 (BitPosition 5):
        # the byte reads 0x01 with the burner off and 0x21 while it fires, so
        # Flamme1 is 0x20. LSB-first would never show the flame and would light
        # a second burner stage this modulating unit does not have.
        mask = 0x80 >> (bit_pos % 8)
        # Where the export ALSO gives a non-zero BytePosition, the two must
        # agree; for a few datapoints they do not (e.g.
        # nviConsumerDmd_Attribute1_CFDM~0xA385 and
        # nvoConsumerDmd_Attribute1_LFDM~0xA346: BitPosition 24 -> byte 3,
        # BytePosition 2). Which one is right cannot be told, so it becomes a
        # comment rather than an entity that reads the wrong byte.
        if ev.byte_position and ev.byte_position != byte_off:
            return CommentSpec(
                [
                    (
                        f"# {name} @ 0x{addr:04X}: BitPosition {bit_pos} implies byte {byte_off} but the "
                        f"export declares BytePosition {ev.byte_position} -- contradictory, needs hardware "
                        "confirmation before it can be emitted"
                    )
                ]
            )
        # binary_sensor reads the block at its base and indexes byte_offset
        # inside it (aligned read); the single-telegram read is the only cap.
        if block_len > MAX_P300_READ_LENGTH or byte_off >= block_len:
            return CommentSpec(
                [
                    (
                        f"# {name} @ 0x{addr:04X}: bit {bit_pos} of a {block_len}-byte block "
                        f"(byte {byte_off}) exceeds binary_sensor length/offset limits -> custom handling"
                    )
                ]
            )
        return EntitySpec(
            platform="binary_sensor",
            name=name,
            address=addr,
            length=block_len,
            byte_offset=byte_off or None,
            bit_mask=mask,
            bit_mask_comment=None,
            disabled_by_default=True,
            update_interval=poll,
            notes=notes,
        )

    if enum_opts:
        enum_len = _enum_read_length(enum_opts, num_length)
        # Aligned block extraction for an interior enum field: read-only, so the
        # block base is simply the read address. The extracted width is
        # enum_len, derived from the option values.
        enum_extract = field_off > 0 and field_off + enum_len <= block_len <= MAX_P300_READ_LENGTH
        return EntitySpec(
            platform="text_sensor",
            platform_type="enum",
            name=name,
            address=addr,
            length=block_len if enum_extract else enum_len,
            byte_offset=field_off if enum_extract else None,
            byte_length=(enum_len if enum_len != 1 else None) if enum_extract else None,
            options=enum_opts,
            option_hex_width=enum_len,
            disabled_by_default=True,
            update_interval=poll,
            notes=notes,
        )

    # Numeric sensor. Signedness: div2/div10 are signed by component default.
    # Every other converter (unsigned by default) needs an explicit
    # `signed: true` when the value range crosses zero; the lower border is the
    # only signal for read-only datapoints (nvoOATemp, Div100, -30..50, would
    # otherwise decode -1.00 as +654.36). force_signed carries a signed default
    # over from a converter-width fallback.
    default_signed = conv in ("div2", "div10")
    lo_hint = ev.lower or (vals[0].lower if vals else "")
    try:
        neg_range = lo_hint != "" and float(lo_hint) < 0
    except (TypeError, ValueError):
        neg_range = False
    # A bounds-inferred encoding has an unsigned raw (0..N); an affine one
    # carries the negative part of the range in the offset filter.
    if bounds_scale is not None:
        neg_range = False

    signed_val = None
    signed_comment = None
    if (force_signed or neg_range) and not default_signed:
        signed_val = True
    elif default_signed and _declared_unsigned(ev):
        # See _declared_unsigned: unsigned type and a non-negative border.
        signed_val = False
        signed_comment = f"Vitosoft ParameterType {getattr(ev, 'parameter_type', '')}, lower border {lo_hint}"

    unit = _unit_for(ev)
    dc = _device_class_for(unit)
    # The device stores the magnitude; German Vitosoft labels the unit "-dBm"
    # (units.py). Unverified on hardware.
    neg_unit = _unit_negated(ev)
    if neg_unit:
        value_filters = list(value_filters) + [("multiply", -1.0)]

    return EntitySpec(
        platform="sensor",
        name=name,
        address=addr,
        length=block_len if use_block_extract else length,
        byte_offset=field_off if use_block_extract else None,
        byte_length=(field_width if field_width != 1 else None) if use_block_extract else None,
        converter=conv,
        signed=signed_val,
        signed_comment=signed_comment,
        unit=unit or None,
        device_class=dc,
        negated_unit_comment=neg_unit,
        filters=value_filters,
        state_class="total_increasing" if conv_kind == COUNTER else "measurement",
        accuracy_decimals=1 if conv_kind == COUNTER else None,
        disabled_by_default=True,
        update_interval=poll,
        notes=notes,
    )


def emit_entity(
    ev: Event,
    profile: str,
    protocol: str = PROTO_P300,
    partial_writes: bool = True,
    ro_blocks: frozenset = frozenset(),
) -> tuple[str, list[str]] | None:
    """Return (platform, yaml_lines) for one event, or None to skip it."""
    spec = build_entity_spec(ev, profile, protocol, partial_writes, ro_blocks)
    if spec is None:
        return None
    if isinstance(spec, CommentSpec):
        return ("comment", spec.lines)
    return (spec.platform, render_entity_lines(spec))
