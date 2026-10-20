"""Data models and Intermediate Representation (IR) for vitocatalog."""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class BoundsScale:
    """Raw encoding inferred from borders + fractional stepping (see
    _scale_from_bounds). value = raw * scale + offset, raw in raw_lo..raw_hi.

    kind: "scale" (offset 0), "affine" (offset = lower border) or "ambiguous"
    (negative lower border outside GWG: signed raw * step and raw * step +
    lower both fit, and nothing in the export decides -> no transform emitted).
    """

    kind: str
    scale: float
    offset: float
    raw_lo: int
    raw_hi: int


@dataclass
class EventValue:
    name: str = ""
    enum_address_value: int | None = None
    enum_replace_value: str = ""
    # ecnEventValueType.Description: the already-resolved German label (e.g.
    # "OK", "Vitodens mit Vitotronic 100 HC1"); needs no Textresource lookup.
    # See _enum_options for the label priority.
    description: str = ""
    # The option's Textresource label in the catalog language ("" for a German
    # catalog) and in German; "" when the text files do not carry it (slim
    # Textresource). See _enum_options for the priority, _enum_bool_pair for
    # why the German one is kept separately.
    label: str = ""
    label_de: str = ""
    unit: str = ""
    unit_token: str = ""  # raw Vitosoft unit token (units.py key)
    lower: str = ""
    upper: str = ""
    stepping: str = ""


@dataclass
class Event:
    id: str
    name: str  # human display (resolved label or technical token)
    address: int | None
    conversion: str
    # Vitosoft access semantics (the DPDefinitions ``Type`` column agrees with
    # the access layer's AccessMode on every row): 1 = read-only, 2 = WRITE-ONLY
    # (reset/trigger registers), 3 = read+write. Writability checks treat {2, 3}
    # as writable; write-only (2) additionally gets no polled state read.
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
    unit_token: str = ""  # its raw Vitosoft token (units.py key)
    tech: str = ""  # stable Viessmann technical identifier (the join key; -> id:)
    token: str = ""  # raw DPDefinitions Address token (e.g. "ecnsysEventType~Error")
    fc_read: str = ""  # access function code for reads (Virtual_READ, GFA_READ, ...)
    fc_write: str = ""  # access function code for writes (Virtual_WRITE, ...)
    # The parameter a Remote_Procedure_Call read is called with (PrefixRead,
    # hex, e.g. "00"): the index of one entry of a list kept behind one
    # address. "" for every other datapoint. See transforms._rpc_prefix().
    prefix_read: str = ""
    # Access-layer value transform: value = raw * conv_factor + conv_offset.
    # None when absent or when the export carries its placeholder pair (see
    # _FACTOR_PLACEHOLDERS). Consumed for MultOffset and for identity
    # conversions with a real factor; never stacked on Div*/Mult* presets.
    conv_factor: float | None = None
    conv_offset: float | None = None
    # Vitosoft's declared value type (`Parameter`: Byte, SByte, Int, SInt, Int4,
    # SInt4, *HighByteFirst, Array, String...). Only a second signal for
    # signedness -- see _declared_unsigned(); it contradicts the borders on 27
    # rows, so it never decides on its own.
    parameter_type: str = ""
    # Access-layer array structure: BlockLength bytes = block_factor records
    # (e.g. ecnsysEventType~Error: BlockLength 90, BlockFactor 10 -> ten 9-byte
    # fault slots). 0/None when the datapoint is not an array.
    block_factor: int | None = None
    values: list[EventValue] = field(default_factory=list)


@dataclass
class CommentSpec:
    """Represents a datapoint that cannot be auto-emitted as an entity."""

    lines: list[str]
    platform: str = "comment"


@dataclass
class EntitySpec:
    """Structured Intermediate Representation for a single vitohome ESPHome entity."""

    platform: str
    name: str
    platform_type: str | None = None
    id: str | None = None
    address: int | None = None
    access: str | None = None
    access_source: str | None = None  # Vitosoft FCRead (/ FCWrite), shown as the access line's comment
    # `prefix:` of an `access: rpc` read: the call's parameter bytes, and the
    # Vitosoft PrefixRead it came from (the line's comment).
    prefix: list[int] | None = None
    prefix_source: str | None = None
    length: int | None = None
    byte_offset: int | None = None
    byte_length: int | None = None
    state_address: int | None = None
    bit_mask: int | None = None
    bit_mask_comment: str | None = None
    converter: str | None = None
    signed: bool | None = None
    signed_comment: str | None = None
    unit: str | None = None
    device_class: str | None = None
    icon: str | None = None
    entity_category: str | None = None
    diagnostic_after_name: bool = False  # entity_category: diagnostic right after name/id (_is_diagnostic)
    on_value: int | None = None
    off_value: int | None = None
    on_label: str = ""
    off_label: str = ""
    min_value: str | float | None = None
    max_value: str | float | None = None
    step: str | float | None = None
    bounds_placeholder_comment: bool = False  # min/max/step are the 0/0/1 placeholders
    mode: str | None = None
    negated_unit_comment: bool = False  # value stored as a magnitude, negated by a filter (_unit_negated)
    filters: list[tuple[str, float]] = field(default_factory=list)
    state_class: str | None = None
    accuracy_decimals: int | None = None
    disabled_by_default: bool = False
    update_interval: int | None = None
    options: list[tuple[int, str]] = field(default_factory=list)
    option_hex_width: int | None = None  # None = decimal keys (bitfields); int = byte width for 0x.. hex keys
    codes: dict[int, str] | None = None
    codes_note: str = ""
    leading_comments: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    trailing_comments: list[str] = field(default_factory=list)
