"""Pure domain rules for datapoint scaling, bitfields, enums, and reachability."""

from __future__ import annotations

import math
import re

import units

from .constants import (
    _BOOLEAN_ON_OFF_PAIRS,
    _CONV_SCALE,
    _STEP_TOL,
    _UNSIGNED_PARAMETER_TYPES,
    COMMAND_STATE_ADDR,
    CONVERSION_MAP,
    COUNTER,
    DIV,
    FCREAD_TO_GWG_ACCESS,
    FCWRITE_TO_GWG_ACCESS,
    MAX_P300_READ_LENGTH,
    MAX_RMW_BLOCK,
    NON_NUMERIC_CONVERSIONS,
    PLAIN,
    POLL_CODING,
    POLL_FAST,
    POLL_SLOW,
    PROTO_GWG,
    PROTO_P300,
    RPC_PREFIX_MAX,
)
from .models import BoundsScale, Event


def _whole_steps(x: float) -> int | None:
    n = round(x)
    return n if abs(x - n) <= _STEP_TOL else None


def _scale_from_bounds(ev: Event, protocol: str = "") -> BoundsScale | None:
    """Infer the raw encoding of an identity-conversion datapoint whose
    borders carry a FRACTIONAL stepping. A plain raw value can only move in
    whole steps, so such a row cannot be `noconv` as declared. None when the
    rule does not apply.

      * lower >= 0 -> "scale": value = raw * step, raw = lower/step ..
        upper/step (e.g. SR13_K17_KonfiKTReglerverstaerkung 0.1..9.9 / 0.1 ->
        raw 1..99). Both borders must be whole steps.
      * lower < 0, GWG -> "affine": value = raw * step + lower, raw 0..N with
        N = (upper - lower) / step. GWG_BEMK55Raumkorrekturwert (EEPROM 0x0016,
        1 byte, -5..5 / 0.125, N = 80) reads 0x28 = 40 on hardware: 40 * 0.125
        - 5 = 0.0 K, the factory default; a signed reading would be +5 K, the
        maximum.
      * lower < 0 elsewhere -> "ambiguous" (e.g. K70_NRP_KonfiOffset-
        Aussentemperatur 0x7770, -5..5 / 0.1: signed raw -50..50 or raw 0..100
        + offset). Nothing decides between the two.

    The ConversionFactor/Offset columns on these rows are NOT the encoding: on
    NoConversion rows they repeat the values of a neighbouring MultOffset row
    (0x0016 carries 5/35 while the bit field at 0x0015 carries 0.125/-5). They
    are ignored when this rule applies.

    Multi-bit fields count their raw range in BitLength bits:
    GWG_BEMK88/89SchaltpunktSPR30L* (two 4-bit fields of 0x001E, 0..1 / 0.1)
    -> raw 0..10 of 15 -> value = field * 0.1 (see _bit_field for the extract).

    Excluded on purpose:
      * single bits (BitLength 1): booleans, never scaled;
      * "Rohwert" rows (GWG_TimerNachlaufRohwert, 0x003A): the RAW companion
        of the MultOffset GWG_TimerNachlauf at the same address, whose 0..100 /
        0.01 borders are copied from the converted sibling.

    UNVERIFIED: every kind is inferred, not declared. The caller emits a
    READ-ONLY sensor -- the component has no affine encode path, and a
    wrong-scale writable number is worse than none.
    """
    if ev.conversion not in ("", "NoConversion"):
        return None
    bit_len = ev.bit_length or 0
    if bit_len == 1:
        return None
    tech = getattr(ev, "tech", "") or ""
    name = getattr(ev, "name", "") or ""
    if "Rohwert" in tech or "Rohwert" in name:
        return None
    width = ev.byte_length or ev.block_length or 1
    if width not in (1, 2, 3, 4):
        return None
    vals = getattr(ev, "values", None) or []
    lo = ev.lower or (vals[0].lower if vals else "")
    hi = ev.upper or (vals[0].upper if vals else "")
    st = ev.stepping or (vals[0].stepping if vals else "")
    try:
        lo_f, hi_f, st_f = float(lo), float(hi), float(st)
    except (TypeError, ValueError):
        return None
    if not all(math.isfinite(x) for x in (lo_f, hi_f, st_f)):
        return None
    # An integral stepping is consistent with a plain raw value; only a
    # fractional one rules raw == value out.
    if st_f <= 0 or st_f == int(st_f) or hi_f <= lo_f:
        return None
    raw_max = (1 << bit_len) - 1 if bit_len > 1 else (1 << (8 * width)) - 1
    if lo_f >= 0:
        r_lo, r_hi = _whole_steps(lo_f / st_f), _whole_steps(hi_f / st_f)
        if r_lo is None or r_hi is None or r_hi > raw_max:
            return None
        return BoundsScale("scale", st_f, 0.0, r_lo, r_hi)
    n = _whole_steps((hi_f - lo_f) / st_f)
    if n is None or not 1 <= n <= raw_max:
        return None
    kind = "affine" if protocol == PROTO_GWG else "ambiguous"
    return BoundsScale(kind, st_f, lo_f, 0, n)


def _bit_field(ev: Event) -> tuple[int, int, int] | None:
    """(byte_offset, shift, mask) of a bit field (BitLength 1..8), so that
    field = (byte >> shift) & mask; None if it does not fit in one byte. The
    component's ``bit_mask`` is ``mask << shift``.

    BitPosition is the absolute MSB-first index of the field's FIRST bit, the
    same numbering as single bits (see the binary_sensor branch of
    build_entity_spec): GWG_K06Kesselmaximaltemperatur (BitPosition 0,
    BitLength 4) is the HIGH nibble of 0x0001, mask 0xF0. A unit reading 0x0001
    = 0xA8 gives K06 = 10 * 5 + 35 = 85 degC and K05 Vorlauf-max HKB (low
    nibble) = 8 * 5 + 35 = 75 degC; LSB-first would give a flow maximum above
    the boiler maximum. Not confirmed by a write.
    """
    bit_len = ev.bit_length or 0
    if not 1 <= bit_len <= 8:
        return None
    bit_pos = ev.bit_position or 0
    shift = 8 - (bit_pos % 8) - bit_len
    if shift < 0:
        return None
    return bit_pos // 8, shift, (1 << bit_len) - 1


def _protocol_for_device(token: str, override: str | None = None) -> str:
    """The protocol a device token targets. Vitosoft names GWG units with a GWG
    family token (GWG_VBES_00, GWG_BT2, HV_GWG, ...); everything else is P300.
    *override* (--protocol) wins, for a token that does not follow the
    convention; --export-all relies on the inference to emit both families in
    one run."""
    if override:
        return override
    return PROTO_GWG if "GWG" in (token or "").upper().split("_") else PROTO_P300


def _is_reachable(ev: Event, protocol: str = PROTO_P300) -> bool:
    """Whether the configured protocol can read this datapoint.

    P300/KW: the only read function code VitoWiFi's datapoint read issues over
    Optolink is Virtual_READ (VS2/P300 FunctionCode 0x01). Datapoints with any
    other access method -- GFA_READ (0x6B), Remote_Procedure_Call (0x07),
    PROZESS_READ (0x7B), KBUS_*/OT_*/Physical_READ -- are not reachable through
    that read: the device NAKs or returns unrelated virtual-space data.

    GWG: Virtual_READ is only a small share of a GWG device's datapoints (6 of
    195 on GWG_VBES_00); the bulk are physical/eeprom/kmbus_eeprom/be/xram/port,
    each reachable on its own access mode via `access:`. So under GWG,
    reachable means "FCRead maps to an access mode we can emit".

    Either way an empty FCRead (no access row) is unknown and kept: on P300 the
    token-derived address is read as a normal virtual read, on GWG on the engine
    default (physical). Dropped datapoints come back with --no-reachable-only.
    Refs: openv "Protokoll 300" and "Protokoll-GWG";
    InsideViessmannVitosoft/VitosoftCommunication.md.
    """
    fc = (getattr(ev, "fc_read", "") or "").strip().lower()
    if protocol == PROTO_GWG:
        return fc == "" or fc in FCREAD_TO_GWG_ACCESS
    return fc in ("", "virtual_read") or _is_rpc_reading(ev, protocol)


def _rpc_prefix(ev: Event, field: str = "prefix_read") -> list[int] | None:
    """The PrefixRead bytes (or PrefixWrite's, with field="prefix_write"); []
    when there is none (a call without a parameter), None when it is
    unparsable or longer than the component accepts. Hex, with or without
    spaces and a 0x (the export writes both "0E" and "0x0E")."""
    text = (getattr(ev, field, "") or "").replace(" ", "")
    if text[:2].lower() == "0x":
        text = text[2:]
    if not text:
        return []
    if len(text) % 2:
        return None
    try:
        out = list(bytes.fromhex(text))
    except ValueError:
        return None
    return out if len(out) <= RPC_PREFIX_MAX else None


def _is_rpc_reading(ev: Event, protocol: str = PROTO_P300) -> bool:
    """Whether a Remote_Procedure_Call datapoint is a READING, emitted with
    `access: rpc` (and `prefix:` when it takes a parameter).

    P300 function code 0x07 covers two different things in the export, and only
    one of them may become an entity:

    * a READING: the procedure answers with a value. Some are called with a
      parameter (PrefixRead) naming one entry of a list -- Teilnehmerliste_LON_00..15
      at 0xA010, sixteen rows sharing one address, prefix 00..0F, declared as a
      6-byte block (VScotHO1_72 answers 5); others with none -- KesselfolgeIst and its siblings on a cascade
      controller, a 4-byte answer.
    * a PROCEDURE: a call with an effect -- RPCWink, valmRPCClearErrorHistoryTable,
      BedienparameterA1M1FunktionReset. Polling one would run it every cycle.
      A parameter says nothing either way (the reset takes the circuit as its
      parameter, the clear takes none); the access mode does: the procedures are
      marked writable (AccessMode 2/3). One row breaks that --
      mBusRPCDeleteReadTableEM2 is marked read-only -- and it is caught by the
      answer size: a procedure answers nothing (BlockLength 0).

    So a reading is: P300, FCRead Remote_Procedure_Call, read-only (access type
    1), not an array (BlockFactor <= 1: a multi-record RPC is one call per index,
    not one entity), an answer of known size (BlockLength/ByteLength > 0), and a
    PrefixRead that is absent or parsable in 1..4 bytes. KW and GWG have no RPC
    telegram at all. Which readings a profile keeps is _is_lon_bound_rpc's
    question.
    """
    if protocol != PROTO_P300:
        return False
    if (getattr(ev, "fc_read", "") or "").strip().lower() != "remote_procedure_call":
        return False
    if ev.access_type in (2, 3):
        return False
    if (ev.block_factor or 0) > 1:
        return False
    if not (ev.block_length or ev.byte_length):
        return False
    return _rpc_prefix(ev) is not None


def _rpc_array_records(ev: Event, protocol: str = PROTO_P300) -> tuple[int, int] | None:
    """(record count, record length) of a Remote_Procedure_Call ARRAY the
    error-history path fans out, one record per call; None for anything else.

    The heat-pump fault buffers are this shape: ecnsysEventType~WPRError at
    0xA801 is BlockLength 240 / BlockFactor 30 (thirty 8-byte records),
    ~WPR3Error 270 / 30 (thirty 9-byte records). A record is not at an address
    of its own: each is one call to the same procedure with the record's index
    as the parameter (0 .. BlockFactor-1). The export gives no PrefixRead for
    them -- the index is the parameter -- so a row that does carry one
    (WPR3MldgsHistExtTn1..3, prefix 01..03 AND 30 records) would need the
    prefix and the index in an order the export does not state, and stays out.

    P300 only (KW and GWG have no RPC), read-only, and a record that fits a
    raw read (MAX_P300_READ_LENGTH).
    """
    if protocol != PROTO_P300:
        return None
    if (getattr(ev, "fc_read", "") or "").strip().lower() != "remote_procedure_call":
        return None
    if ev.access_type != 1 or getattr(ev, "prefix_read", ""):
        return None
    count = ev.block_factor or 0
    total = ev.block_length or ev.byte_length or 0
    if count <= 1 or total <= 0 or total % count:
        return None
    length = total // count
    return (count, length) if length <= MAX_P300_READ_LENGTH else None


def _is_rpc_action(ev: Event, protocol: str = PROTO_P300) -> bool:
    """Whether a Remote_Procedure_Call datapoint is an ACTION, emitted as a
    `type: action` button: a procedure that a press runs, with PrefixWrite as
    its parameter and nothing else to send.

    The procedures are the writable RPC rows (AccessMode 2/3), and a call to
    one is the same telegram as an RPC read -- function code 0x07 plus the
    parameter; BlockLength is the size of the answer, not a value. What a
    button cannot do is supply a value, so a procedure that takes one stays
    out. The export tells them apart this way:

    * answers nothing (BlockLength 0): a trigger, whatever else the row says --
      valmRPCClearErrorHistoryTable, RPCWink, KesselfolgeFunktionNext,
      vlogRPCClearMemberList;
    * write-only (AccessMode 2), a one-byte `Array`, no borders, no unit: a
      trigger too -- the circuit and coding resets
      (BedienparameterA1M1FunktionReset, Konfi_FunktionReset_GWG), the heat-pump
      resets and restart;
    * everything else carries a value: borders (RPCLesenFehlertabelle 0..74),
      a unit (RPCKonfigTrendingZeitintervall*, seconds), a numeric type
      (RPCSetImpulseCounter*, Int4), or a read+write row that answers
      (RPCSetDigitalOutput*).

    P300 only, not an array, and a PrefixWrite that parses.
    """
    if protocol != PROTO_P300:
        return False
    if (getattr(ev, "fc_write", "") or "").strip().lower() != "remote_procedure_call":
        return False
    if ev.access_type not in (2, 3):
        return False
    if (ev.block_factor or 0) > 1:
        return False
    if _rpc_prefix(ev, "prefix_write") is None:
        return False
    answer = ev.block_length or ev.byte_length or 0
    if answer == 0:
        return True
    return (
        ev.access_type == 2
        and answer <= 1
        and (ev.parameter_type or "").lower() == "array"
        and not (ev.lower or ev.upper or ev.unit_token)
    )


def _access_for(ev: Event, protocol: str) -> str:
    """The `access:` value to emit for one event, or "" when none is emitted
    (KW, P300 other than an RPC reading, or GWG with a blank FCRead, where the
    engine default already is physical)."""
    if _is_rpc_reading(ev, protocol):
        return "rpc"
    if protocol != PROTO_GWG:
        return ""
    return FCREAD_TO_GWG_ACCESS.get((getattr(ev, "fc_read", "") or "").strip().lower(), "")


def _is_writable(ev: Event, protocol: str = PROTO_P300) -> bool:
    """Write-side counterpart of _is_reachable.

    P300/KW: a datapoint Vitosoft marks writable (access type 2/3) is settable
    over Optolink only when its write function code is Virtual_WRITE, the only
    write VitoWiFi issues. FCWrite "undefined", GFA_WRITE,
    Remote_Procedure_Call etc. mean the write would not take, so the datapoint
    becomes a read-only sensor instead of a number/select. A blank FCWrite is
    unknown and trusted to the access type.

    GWG: writable when Vitosoft marks it so AND its FCWrite names a mode with a
    write TYPE byte AND that mode is the one its reads use. `access:` is a
    single option driving both directions, so a datapoint whose read and write
    modes disagreed could not be expressed. Vitosoft pairs them within one mode
    (EEPROM_READ / EEPROM_WRITE, BE_READ / BE_WRITE), so the last condition
    drops nothing in the current export.
    """
    if protocol == PROTO_GWG:
        if ev.access_type not in (2, 3):
            return False
        write_access = FCWRITE_TO_GWG_ACCESS.get((getattr(ev, "fc_write", "") or "").strip().lower())
        if write_access is None:
            return False
        return write_access == _access_for(ev, PROTO_GWG)
    if ev.access_type not in (2, 3):
        return False
    return (getattr(ev, "fc_write", "") or "").strip().lower() in ("", "virtual_write")


def _poll_for(ev: Event, conv_kind: str, protocol: str = PROTO_P300) -> int:
    if _is_writable(ev, protocol):
        return POLL_CODING
    if conv_kind == COUNTER:
        return POLL_SLOW
    return POLL_FAST


def _enum_options(ev: Event, german: bool = False) -> list[tuple[int, str]]:
    """Return [(raw_value, label)] from the event's value types, if any.

    Label priority: the option's Textresource label in the catalog language
    (never for German) -> Description (pre-resolved German) -> the German
    Textresource label -> resolved EnumReplaceValue -> Name -> hex.
    ``german=True`` skips the first step.

    German keeps Description first: it is the German text already, and it is
    often more specific than the shared UI label ("1 M2 setzt Anforderung an
    Int.Pumpe" vs "1: HK setzt ..."), which also carries hyphenation artefacts
    ("Betriebsarten-umschaltung"). The UI label only fills the ~13% of options
    without a Description. Other languages have only the translation.

    Skipped options:
      * a negative value (some value types, e.g. SNVTAlarm_*, WPR3_Mischer_*,
        carry EnumAddressValue -1): the raw bytes compare unsigned, so it can
        never match, and a `0x-1` key fails `esphome config`; the caller adds a
        NOTE with the count (_negative_option_count);
      * a value already seen: the export can map two options to the same value
        (nvoNodeAlarm at 0x82E has value 3 for both "service alarm 2" and
        "service alarm 3"), and duplicate option keys fail `esphome config`.
        The first in document order is kept.

    getattr: hand-built stand-ins in tests may lack the label fields.
    """
    out: list[tuple[int, str]] = []
    seen_values: set[int] = set()
    for v in getattr(ev, "values", None) or ():
        val = getattr(v, "enum_address_value", None)
        if val is None or val < 0 or val in seen_values:
            continue
        seen_values.add(val)
        shown = "" if german else getattr(v, "label", "")
        label = (
            shown
            or getattr(v, "description", "")
            or getattr(v, "label_de", "")
            or getattr(v, "enum_replace_value", "")
            or getattr(v, "name", "")
            or f"0x{val:02X}"
        )
        out.append((val, label))
    return out


def _negative_option_count(ev: Event) -> int:
    """How many enum options _enum_options dropped for being negative."""
    return sum(
        1
        for v in (getattr(ev, "values", None) or ())
        if getattr(v, "enum_address_value", None) is not None and v.enum_address_value < 0
    )


def _dedup_option_labels(opts: list[tuple[int, str]]) -> list[tuple[int, str]]:
    """Disambiguate repeated option labels by appending their raw hex value.

    ESPHome `select` (and the enum text_sensor, for consistency) require UNIQUE
    option labels, but the export can give several values the same label --
    e.g. KF1_KonfiTemperaturprogramm has values 6..15 all labelled "Default".
    A repeated label gets its raw value appended ("Default (0x06)"); the first
    occurrence stays as is. Order and values are preserved.
    """
    seen: dict[str, int] = {}
    result: list[tuple[int, str]] = []
    for value, label in opts:
        if label in seen and seen[label] != value:
            result.append((value, f"{label} (0x{value:02X})"))
        else:
            seen[label] = value
            result.append((value, label))
    return result


def _norm_bool_label(label: str) -> str:
    """Normalize an enum option label for on/off matching: drop a leading
    coding-value echo ("0 inaktiv", "1: Ja"), lowercase, trim."""
    return re.sub(r"^\d+\s*[:.]?\s*", "", (label or "").strip()).lower()


def _boolean_pair(opts: list[tuple[int, str]]) -> tuple[int, int, str, str] | None:
    """(on_value, off_value, on_label, off_label) when a 2-option enum is a
    semantic on/off toggle, else None.

    Two options alone do NOT make a boolean: K01 {Einkessel, Mehrkessel} or
    K88 {Celsius, Fahrenheit} are choices, and a switch showing "on" would be
    meaningless for them. A pair qualifies only via:
      * an unresolved Allgemein_Zustand_Ein_Aus~N token stem (the Neptun pump
        overrides), where ~1 is on and ~0 is off by the type's own name;
      * a negation pair, one label being "nicht " + the other
        (vorhanden / nicht vorhanden, senden / nicht senden);
      * a known on/off label pair after normalization (EIN/AUS, aktiv/inaktiv,
        Ja/Nein, ...), matched in either value order -- so K8A's
        {175: aktiv, 176: inaktiv} works despite its non-0/1 values.
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
            on_v, _on_l = pick["1"]
            off_v, _off_l = pick["0"]
            return (on_v, off_v, "Ein", "Aus")
    a, b = _norm_bool_label(raw_a), _norm_bool_label(raw_b)
    # Negation rule.
    if a == "nicht " + b:
        return (v_b, v_a, raw_b, raw_a)
    if b == "nicht " + a:
        return (v_a, v_b, raw_a, raw_b)
    # Known pairs, either order.
    for on_word, off_word in _BOOLEAN_ON_OFF_PAIRS:
        if a == on_word and b == off_word:
            return (v_a, v_b, raw_a, raw_b)
        if a == off_word and b == on_word:
            return (v_b, v_a, raw_b, raw_a)
    return None


def _enum_bool_pair(ev: Event, enum_opts: list[tuple[int, str]]) -> tuple[int, int, str, str] | None:
    """_boolean_pair for *ev*, decided on its GERMAN option labels and returned
    with the display labels of *enum_opts*.

    _boolean_pair recognises German and English on/off words only; deciding on
    the catalog language would make an entity a switch in a German catalog and
    a select in a French one. German is the language every label exists in."""
    pair = _boolean_pair(_dedup_option_labels(_enum_options(ev, german=True)))
    if pair is None:
        return None
    on_v, off_v, on_l, off_l = pair
    shown = dict(enum_opts)
    return (on_v, off_v, shown.get(on_v, on_l), shown.get(off_v, off_l))


def _enum_read_length(enum_opts: list[tuple[int, str]], fallback: int) -> int:
    """Minimal byte width that holds every enum value. A status enum in a larger
    block (e.g. SM1_Sensor_*_Status_GWG, a 1-byte field inside the 20-byte
    0xCF90 block) must read 1 byte at its BytePosition, not the clamped block
    width -- a 4-byte read never matches 0/1/2/128. Derived from the values;
    *fallback* (the clamped length) only if there are none."""
    if not enum_opts:
        return fallback
    hi = max(v for v, _ in enum_opts)
    return 1 if hi < 0x100 else (2 if hi < 0x10000 else fallback)


def _device_class_for(unit: str) -> str | None:
    """Home Assistant device_class for a unit SYMBOL (units.py); None for units
    that do not fix the quantity (%, K deltas, m³, ...)."""
    return units.DEVICE_CLASS_BY_SYMBOL.get(unit or "")


def _unit_for(ev: Event) -> str:
    unit = ""
    for v in getattr(ev, "values", None) or ():
        if getattr(v, "unit", ""):
            unit = v.unit
            break
    unit = unit or getattr(ev, "unit", "") or ""
    # HA's canonical pressure spelling is "mbar"; the export writes "mBar".
    return "mbar" if unit == "mBar" else unit


def _unit_negated(ev: Event) -> bool:
    """Whether the stored value is a magnitude whose sign lives in the unit
    (units.py `negate`: the RF level bytes, German label "-dBm")."""
    token = ""
    for v in getattr(ev, "values", None) or ():
        if getattr(v, "unit_token", ""):
            token = v.unit_token
            break
    row = units.lookup(token or getattr(ev, "unit_token", ""))
    return bool(row and row[2])


def _declared_unsigned(ev: Event) -> bool:
    """Both signals say the raw value is unsigned: the declared ParameterType
    is Byte/Int/Int4 AND the lower border is present and >= 0.

    The component decodes div2/div10 as SIGNED by default (a sub-zero
    temperature must read correctly). For a datapoint Vitosoft declares
    unsigned, that is wrong above half the range: GWG_Kesseltemperatur (0x0070,
    1 byte, Div2, Int, 0..127.5 degC) at 70 degC is raw 0x8C, which a signed
    decode publishes as -58 degC.

    ParameterType alone is not trustworthy: it contradicts the borders on 27
    rows, among them the outdoor sensor ADC_IstTemperaturwert ~0x0800 (declared
    Int, bordered -60..60) -- decoding that one unsigned would publish 6553.5
    degC at -0.1 degC. Where the lower border is negative, absent or
    unparsable, the converter default / negative-border rule stands.
    """
    ptype = (getattr(ev, "parameter_type", "") or "").strip().lower()
    if ptype not in _UNSIGNED_PARAMETER_TYPES:
        return False
    vals = getattr(ev, "values", None) or []
    lo = ev.lower or (vals[0].lower if vals else "")
    try:
        return float(lo) >= 0
    except (TypeError, ValueError):
        return False


def _llround(x: float) -> int:
    """Round half away from zero -- the semantics of C++ ``std::llround`` and of
    the component's ``llround`` helper. Python's built-in ``round()`` is half to
    even and diverges at negative half-steps (round(-128.5) == -128 fits int8;
    llround(-128.5) == -129 is rejected)."""
    return int(math.floor(abs(x) + 0.5)) * (1 if x >= 0 else -1)


def _bound_fits(value_str: str, length: int, conv: str, is_signed: bool) -> bool:
    """Whether a min/max border survives encoding into ``length`` raw bytes.

    Mirrors components/vitohome/number.py (raw = llround(value / scale) -- half
    away from zero, matching ``decode.h::encode_scaled`` -- then a
    signed/unsigned range check), so a border the generator emits is not
    rejected at ``esphome config`` time. Borders that do not fit (e.g. a
    year-valued bound on a 1-byte field, or a display unit that is not the raw
    encoding) fall back to the 0/0/1 placeholder instead.
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


def _read_only_blocks(events, protocol: str) -> frozenset[tuple[int, str]]:
    """(block base, access) of every block that holds a READ-ONLY datapoint.

    Such a block carries live data: a whole-block write would restore it from a
    stale read, so its writable fields stay read-only (_block_write_blocker).
    Computed over ALL of the device's datapoints, before any profile or name
    filter, and conservatively: a datapoint demoted for an unusable write
    function code counts as read-only too. Keyed like the GWG field identity:
    an address is per access mode there.
    """
    return frozenset(
        (e.address, _access_for(e, protocol)) for e in events if e.address is not None and not _is_writable(e, protocol)
    )


def _block_write_blocker(
    ev: Event,
    protocol: str,
    block_len: int,
    field_width: int,
    ro_blocks: frozenset,
    partial_writes: bool,
) -> str | None:
    """Why a writable field smaller than its block can NOT be written as a
    whole-block read-modify-write, or None if it can.

    The device writes its whole datapoint: a 1-byte write into a 2-byte block
    fills the second byte from the telegram (its checksum byte). Such a field is
    therefore written by reading the block, replacing the field and writing all
    of it back (component block mode, docs/design_notes.md SS5b). That is safe
    only for a block that holds settings: a block that also carries a READ-ONLY
    datapoint holds live data, and writing it back would restore a value the
    device may have changed since the read.
    """
    if not partial_writes:
        return "partial writes disabled (--no-partial-writes)"
    if not 1 <= field_width <= 4:
        return f"a {field_width}-byte field is not a scalar"
    if block_len > MAX_RMW_BLOCK:
        return f"the {block_len}-byte block exceeds the {MAX_RMW_BLOCK}-byte read-modify-write"
    if (ev.address, _access_for(ev, protocol)) in ro_blocks:
        return "the block also holds read-only (live) data, which a block write would restore from a stale read"
    if COMMAND_STATE_ADDR.get(ev.address) is not None:
        return "command register with a separate state address"
    return None


_LON_INPUT_RE = re.compile(r"^nvi[A-Z]")


def _is_lon_input(ev: Event) -> bool:
    """A LON network-variable INPUT (``nvi*``, e.g. nviBoilerCmd_BLR_value).

    The controller mirrors its LON objects into memory that Optolink reads, so
    these answer -- but an input is written by a LON master, and without a LON
    module nothing ever does: it reads as the untouched default with LON's
    "invalid" marker (e.g. 0xA300 nviBoilerCmd reads ``00 FF``). Outputs (nvo*,
    the controller's own state, e.g. nvoAlarm) and configuration (nci*) are
    unaffected. Many inputs are writable, and writing one over Optolink may
    stand in for a LON master, so --profile full keeps them.
    """
    tok = getattr(ev, "token", "") or getattr(ev, "tech", "") or ""
    return bool(_LON_INPUT_RE.match(tok.split("~", 1)[0]))


# The LON participant procedure. Every reading at this address is one entry of
# the LON participant list (Teilnehmerliste_LON_00..15) or the unit's own LON
# identity (Vitocom_Node, _Subnet, _Neuron_ID).
_LON_RPC_ADDRESS = 0xA010
_LON_RPC_INPUT_RE = re.compile(r"nvi[A-Z]")


def _is_lon_bound_rpc(ev: Event) -> bool:
    """A remote procedure reading that only a LON module gives meaning to.

    The LON participant list and LON identity (_LON_RPC_ADDRESS), a LON
    module's own procedures (`vlon*`, e.g. vlonRPCGetBinding) and LON inputs read
    by procedure (ddcRPCnvi*, the RPC counterpart of _is_lon_input). Without a
    LON module they are still answered but hold only the unit's own defaults
    (VScotHO1_72: own number 01/01, the other entries empty), so the profiles
    treat them like the LON inputs (_profile_keep). The other readings -- a cascade controller's
    boiler sequence, an M-Bus module's state -- belong to the unit itself.
    """
    if not _is_rpc_reading(ev):
        return False
    if ev.address == _LON_RPC_ADDRESS:
        return True
    stem = (getattr(ev, "token", "") or getattr(ev, "tech", "") or "").split("~", 1)[0]
    return stem.lower().startswith("vlon") or bool(_LON_RPC_INPUT_RE.search(stem))


def _profile_keep(ev: Event, profile: str) -> bool:
    if profile == "full":
        return True
    # minimal and standard: no LON inputs (_is_lon_input), and no LON-bound
    # remote procedure readings (_is_lon_bound_rpc).
    if _is_lon_input(ev) or _is_lon_bound_rpc(ev):
        return False
    _conv, conv_kind = CONVERSION_MAP.get(ev.conversion, (None, PLAIN))
    writable = ev.access_type in (2, 3)
    if profile == "standard":
        return ev.conversion not in NON_NUMERIC_CONVERSIONS
    if profile == "minimal":
        if writable:
            return True
        return conv_kind in (DIV, COUNTER)
    return True


# Fault / status / health registers, tagged entity_category: diagnostic so they
# group under Diagnostics in Home Assistant rather than mixing with
# measurements. Covers fault bytes (GFA 0x5738, EEPROM/I2C), the LON alarm
# record (0xA132), sensor-status enums (TemperaturFehler_*, *Sensor*Status*,
# 0x089C.., 0xCF90), collective-fault flags (Sammel*), and the system /
# maintenance status enums.
_DIAGNOSTIC_RE = re.compile(
    r"Fehler|Stoer|St[oö]r|Status|Sensor.*Status|SensorStatus|Alarm|EEPROM|I2C|"
    r"Diagnos|Sammel|Wartung|Quitt|TemperaturFehler|ecnStatusEventType",
    re.IGNORECASE,
)


def _is_diagnostic(ev: Event) -> bool:
    target = getattr(ev, "token", "") or getattr(ev, "tech", "") or getattr(ev, "name", "") or ""
    return bool(_DIAGNOSTIC_RE.search(target))


def _is_mode_select(ev: Event) -> bool:
    target = getattr(ev, "token", "") or getattr(ev, "tech", "") or ""
    return ev.access_type in (2, 3) and bool(re.search(r"Betriebsart|BedienteilBA|BedienBetriebsart", target, re.IGNORECASE))
