"""ESPHome component for Viessmann Optolink (vendored optolink engine).

P300 (VS2) is the validated protocol; KW (VS1) and GWG are build-time selectable
through the same engine template (protocol_select.h) but untested. Platforms:
sensor, binary_sensor, text_sensor, number, select, switch, text, button,
climate and event. The component decodes and encodes raw Optolink payloads
itself (see ``decode.h``) and uses the in-tree optolink engine (under
``optolink/``) only as the wire/transport layer; the engine's converters are
never exercised (every ``Datapoint`` is built with ``optolink::noconv`` and the
raw-bytes write overload is used).

Why the component decodes rather than using engine converters -- upstream ships
a converter layer, but the vendored copy removed it (THIRD_PARTY.md 13/15)
precisely because:
  * its accessor returned a tagless union with no record of which member was
    written, so reading the wrong member silently returned garbage; and
  * it did all converter math in float32, which loses precision for 4-byte
    counters (uint32 -> float drops bits above 2**24).
``decode.h`` extracts the integer in int64/uint64, scales in double, and only
narrows the *final* value to the float32 ESPHome state requires.
"""

from dataclasses import dataclass
import logging
import math
from pathlib import Path

import esphome.codegen as cg
from esphome.components import esp32, time as time_, uart
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_INTERVAL, CONF_LENGTH, CONF_NAME, CONF_PROTOCOL, CONF_TIME_ID, CONF_UPDATE_INTERVAL
from esphome.core import CORE, ID, CoroPriority, coroutine_with_priority
import esphome.final_validate as fv

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@SoulSolistice"]
DEPENDENCIES = ["uart"]
# This component ships platform entities (sensor, binary_sensor, ...), but it
# deliberately does NOT AUTO_LOAD their base components. Each
# vito_<platform>.{h,cpp} guards its body with #ifdef USE_<PLATFORM>, and the hub
# guards the few platform-typed members it owns (link binary_sensors, device-id
# and scan-result text_sensors) the same way. A base is therefore pulled in --
# and USE_<PLATFORM> defined -- only when the user actually configures that
# platform via its own `sensor:` / `binary_sensor:` / ... block, so a device
# config compiles just the platforms it uses instead of all ten. Forcing all
# platform bases in via AUTO_LOAD is disallowed by the ESPHome component
# guidelines for exactly this reason.
MULTI_CONF = False

CONF_VITOHOME_ID = "vitohome_id"
CONF_IDENTIFY_DEVICE = "identify_device"
# Optional in-component Optolink frame logging (hub-level). See
# vito_uart_interface.h: the adapter already knows where a telegram starts
# and ends, so this needs no `uart: debug:` block and no `after:` delimiter
# rule. The `delimiter: [0x06]` recipe that circulates for Optolink is a P300
# ACK byte and is an ordinary data byte on KW, where it tears frames apart.
CONF_LOG_FRAMES = "log_frames"
# Interactive scan-console lane capacity; see
# VitoHomeComponent::set_raw_queue_capacity for the RAM trade-off. Defaults to
# 0 -- clock sync does not use this lane.
CONF_RAW_QUEUE_SIZE = "raw_queue_size"

# System-time sync options (hub-level; see VitoHomeComponent::set_time_sync).
CONF_TIME_SYNC = "time_sync"
CONF_CLOCK_ADDRESS = "clock_address"
CONF_DRIFT_THRESHOLD = "drift_threshold"
CONF_SYNC_ON_BOOT = "sync_on_boot"

# Defaults are deliberately conservative: a daily check, a one-minute drift
# tolerance, and a one-shot sync once the time source is first valid. All three
# are user-overridable. interval: 0s disables the periodic check (boot-only).
TIME_SYNC_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_INTERVAL, default="24h"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_DRIFT_THRESHOLD, default="60s"): cv.positive_time_period_seconds,
        cv.Optional(CONF_SYNC_ON_BOOT, default=True): cv.boolean,
        # The device clock datapoint. NOT a constant across Viessmann devices --
        # source-confirmed against the Vitosoft DPDefinitions.xml link tables,
        # which carry three distinct schemes:
        #
        #   NRF_Uhrzeit~0x088E   8-byte DateTimeBCD. The default here, and the
        #                        address openv/vcontrold document for the
        #                        Vitotronic family. Hardware-confirmed on a
        #                        Vitodens 300-W (B3HA).
        #   WPR_Uhrzeit~0x08E0   8-byte DateTimeBCD, but a DIFFERENT address --
        #                        the WPR heat-pump controllers (V200WO1A,
        #                        VBC700_AW, VBC700_BW_WW, VBC702_AW, VBC702_S,
        #                        CU401B_A/G/S). Set clock_address: 0x08E0 there.
        #   GWG_Uhrzeit_*        no BCD blob at all: three separate 1-byte
        #                        registers (0x0074 weekday / 0x0075 hour /
        #                        0x0076 minute). NOT reachable by changing this
        #                        option -- the 8-byte read/write shape is wrong
        #                        for it. _final_validate rejects time sync under
        #                        GWG outright rather than let this look
        #                        configurable when it isn't.
        #
        # Why an option and not a lookup from the device ident: only 16 of the
        # 399 datapoint-type tokens list ANY clock datapoint in the XML, and the
        # Vitodens 300-W token (VScotHO1_72) is NOT one of them even though
        # 0x088E demonstrably works on it. A per-token lookup would therefore
        # answer "unknown" for the overwhelming majority of real devices,
        # including the reference unit. The XML is authoritative that 0x08E0
        # exists and differs; it is not authoritative that 0x088E is right
        # everywhere else.
        #
        # 8 bytes of BCD is assumed regardless (VitoClock::CLOCK_LEN): both
        # DateTimeBCD variants are 8 bytes, only the address moves.
        cv.Optional(CONF_CLOCK_ADDRESS, default=0x088E): cv.hex_uint16_t,
    }
)

# Shared platform option names. Centralised here (rather than redefined in each
# platform file) so a single string change propagates to every consumer -- the
# same reason ESPHome core hoists shared CONF_ keys into components/const. The
# per-platform validation of these keys (e.g. the byte_length int_range, which
# differs 1..4 vs 1..2 by platform) stays in each platform's schema; only the
# option *name* is shared.
#
# CONF_LENGTH and CONF_PROTOCOL are NOT defined here: they already exist in
# esphome.const with identical values, so they are imported above and re-exported
# through this module like the rest. Defining a second literal for a key ESPHome
# core already owns is how the two drift apart. Everything below is genuinely
# component-specific and has no upstream counterpart.
CONF_CONVERTER = "converter"
CONF_SIGNED = "signed"
CONF_READ_BACK = "read_back"
CONF_STATE_ADDRESS = "state_address"
CONF_BYTE_OFFSET = "byte_offset"
CONF_BYTE_LENGTH = "byte_length"
# GWG access mode (2026-08-24). Only meaningful under `protocol: GWG` --
# enforced in _final_validate below, the same place the single-byte GWG
# address constraint already lives, since both need the full config visible
# to know the hub's protocol.
#
# Wired into every addressed platform (_ACCESS_DOMAINS). A GWG datapoint's
# access mode selects the TYPE byte, and GWG addresses are per-mode, so
# `address:` alone is ambiguous -- Vitosoft's own GWG tables have five
# different datapoints at address 0x01 across four modes.
#
# ONE mode drives both directions. Vitosoft pairs every writable GWG datapoint
# as (EEPROM_READ, EEPROM_WRITE) or (BE_READ, BE_WRITE), never across modes, so
# a separate write_access would be a way to express only wrong things. The two
# KMBUS modes have no write TYPE byte at all; the engine refuses a write in
# those rather than silently substituting another mode.
CONF_ACCESS = "access"

# Platforms whose schema accepts `access:`. Kept here so _final_validate and
# the platform modules cannot drift apart.
_ACCESS_DOMAINS = ("sensor", "binary_sensor", "text_sensor", "number", "select", "switch", "text", "climate")

# The subset of those that WRITE. GWGEngine::write() permanently refuses an
# access mode with no write telegram type, and the write lane's response to a
# permanent refusal is to drop the item and log -- so such an entity would
# compile, publish, accept a value from Home Assistant, and silently never
# write it. Rejected at config time instead (see _final_validate).
_WRITE_DOMAINS = ("number", "select", "switch", "text", "climate")

# Access modes with no write telegram type in the openv wiki's Protokoll-GWG
# table -- mirrors gwgWriteTypeByte()/gwgModeIsWritable() in constants.h, which
# return kNoGwgWriteType for exactly these two.
_READ_ONLY_ACCESS_MODES = ("kmbus_ram", "kmbus_eeprom")

vitohome_ns = cg.esphome_ns.namespace("vitohome")
optolink_ns = vitohome_ns.namespace("optolink")

# GWG access mode (2026-08-24). Mirrors optolink/constants.h::GWGAccessMode
# exactly -- name, order and member spelling all have to match by hand since
# this is a Python-side reference to a C++ enum class, not a generated
# binding; a mismatch here fails at C++ compile time (unknown enumerator),
# not at `esphome config` time. See constants.h for the evidence behind each
# mode's TYPE byte (openv wiki, Protokoll-GWG).
GWGAccessMode = optolink_ns.enum("GWGAccessMode", is_class=True)
GWG_ACCESS_MODES = {
    "physical": GWGAccessMode.PHYSICAL,
    "virtual": GWGAccessMode.VIRTUAL,
    "eeprom": GWGAccessMode.EEPROM,
    "xram": GWGAccessMode.XRAM,
    "port": GWGAccessMode.PORT,
    "be": GWGAccessMode.BE,
    "kmbus_ram": GWGAccessMode.KMBUS_RAM,
    "kmbus_eeprom": GWGAccessMode.KMBUS_EEPROM,
}

# One {wire value -> label} row of a lookup table (vito_entity.h). Emitted as a
# `static const VitoOption name[] = {...}` array; see emit_option_table.
VitoOption = vitohome_ns.struct("VitoOption")

# One Betriebsart preset row (vito_climate.h). Emitted as a
# `static const VitoClimatePreset name[] = {...}` array; see
# climate.py::_emit_preset_table.
VitoClimatePreset = vitohome_ns.struct("VitoClimatePreset")

VitoHomeComponent = vitohome_ns.class_("VitoHomeComponent", cg.PollingComponent, uart.UARTDevice)

# Selectable protocols. P300 (VS2) is the only one exercised on hardware; KW
# (VS1) and GWG build through the same engine template but untested -- selecting
# either emits a warning at compile time. The value is the build-flag token
# (P300/KW/GWG) that selects the engine via protocol_select.h.
PROTOCOLS = {
    "P300": "P300",
    "VS2": "P300",
    "KW": "KW",
    "VS1": "KW",
    "GWG": "GWG",
}


@dataclass(frozen=True)
class Converter:
    """A named decode/encode preset.

    ``scale`` is the multiplier applied to the raw integer (raw * scale = value);
    ``default_signed`` is whether the raw integer is interpreted as two's
    complement when no explicit ``signed:`` is given; ``lengths`` is the set of
    payload byte-lengths that make physical sense for the preset; ``encodable``
    marks presets usable on a write path (``number`` / ``select``).

    Sign defaults follow the Viessmann Vitosoft conversions verified against
    VitoWiFi at the pinned commit: ``Div2`` and ``Div10`` are signed (so a
    sub-zero temperature decodes correctly), everything else is unsigned.

    Note: because the component decodes the payload itself, these length sets are about what is *physically sensible and float32-safe
    after scaling*, not about VitoWiFi's internal asserts. The values that are
    still load-bearing are the per-``number`` encodable-range checks in
    ``number.py`` (a raw value that does not fit the byte width is rejected at
    ``esphome config`` time).
    """

    scale: float
    default_signed: bool
    lengths: tuple
    encodable: bool
    # RotateBytes: the same bytes assembled big-endian (read_be in decode.h).
    big_endian: bool = False


CONVERTERS = {
    # name           scale         signed  lengths        encodable
    "noconv": Converter(1.0, False, (1, 2, 3, 4), True),
    "div2": Converter(0.5, True, (1, 2), True),
    "div10": Converter(0.1, True, (1, 2), True),
    "div100": Converter(0.01, False, (1, 2, 4), True),
    "div1000": Converter(0.001, False, (2, 4), True),
    # Sec2Hour (value / 3600). Read-only: nobody writes an hours counter, and
    # the seconds value of a 4-byte counter exceeds float32's exact range
    # (which is exactly why decode happens in double here, not float32).
    "sec2hour": Converter(1.0 / 3600.0, False, (4,), False),
    "mult2": Converter(2.0, False, (1, 2, 4), True),
    "mult5": Converter(5.0, False, (1, 2, 4), True),
    "mult10": Converter(10.0, False, (1, 2, 4), True),
    "mult100": Converter(100.0, False, (1, 2, 4), True),
    # RotateBytes: big-endian 2-byte coding values (GWG_Codierstecker_Kennziffer,
    # VSKO_Scot_NEC_*). Read-only; decoded via decode_scaled_be.
    "rotatebytes": Converter(1.0, False, (2,), False, big_endian=True),
}


def converter_scale(name: str) -> float:
    return CONVERTERS[name].scale


def scale_literal(name: str) -> cg.RawExpression:
    """The converter scale as a C++ *double* literal expression.

    Passing the Python float straight into codegen makes ESPHome emit a float32
    literal (``set_scale(0.1f)``), quantizing the scale constant *before* the
    decode path's double multiply -- for div10/div100/div1000/sec2hour a large
    share of raw values then publish a float32 one ULP off the correctly-rounded
    value, silently narrowing the "read in uint64, scale in double, narrow last"
    guarantee. ``repr()`` of a Python float is the shortest decimal that
    round-trips, and C++ parses an unsuffixed literal as double, so the C++
    constant is bit-identical to the Python double.
    """
    return cg.RawExpression(repr(float(CONVERTERS[name].scale)))


def llround(x: float) -> int:
    """Round half away from zero -- the exact semantics of C++ ``std::llround``.

    Python's built-in ``round()`` is banker's rounding (half to even), which
    diverges from ``decode.h::encode_scaled`` at negative half-steps: e.g.
    ``round(-128.5) == -128`` (fits int8) while ``llround(-128.5) == -129``
    (rejected at runtime). Any config-time check that claims to mirror the C++
    encode path must use this, not ``round()``.
    """
    return int(math.floor(abs(x) + 0.5)) * (1 if x >= 0 else -1)


def converter_big_endian(name: str) -> bool:
    return CONVERTERS[name].big_endian


def converter_default_signed(name: str) -> bool:
    return CONVERTERS[name].default_signed


# Widest BLOCK READ this component will issue in one telegram.
#
# EVIDENCE. A 42-byte read succeeds on P300 (VScotHO1_72, 2026-07-10):
#     >>> 41:05:00:01:73:60:2A:03                (read 0x7360, 0x2A = 42 bytes)
#     <<< 06:41:2F:01:01:73:60:2A:<42 bytes>:2B  (MessageIdentifier 0x01)
# 22 and 32 were proven the same day. Nothing wider has been tried.
#
# THE OLD VALUE, 37, WAS FICTION. It was introduced here as "the widely-cited
# safe maximum" with no citation, and nothing supports it:
#   * openv's Protokoll 300 spec documents the length byte as the count of bytes
#     between 0x41 and the checksum, and names NO maximum read length.
#     https://github.com/openv/openv/wiki/Protokoll-300
#   * vcontrold defines no read wider than 9 bytes -- its own documented
#     limitation, not the protocol's.
#     https://github.com/openv/openv/wiki/vcontrold.xml
#   * The one hardware observation ever cited for it -- a 40-byte read at 0x7362
#     erroring on P300 -- was a misdiagnosis. 0x7362 is not a datapoint; it is
#     the block base 0x7360 plus BytePosition 2, an address this generator
#     fabricated. A 2-byte read there fails identically.
# Where 37 probably came from: the response to a 32-byte read opens `41:25:...`,
# and 0x25 = 37, because the P300 length byte counts 5 + payload. Someone read a
# telegram length byte as a data length. Speculative, but it is the only account
# of the number that fits an observed frame.
#
# 48 is chosen as the ceiling we are willing to ATTEMPT: it covers every block
# the catalogs emit (the widest is the 42-byte Beschriftung_* label block, which
# is proven), and it matches VitoHomeComponent::RAW_READ_MAX so the raw scan
# console can test any block read before you enable the entity that performs it.
# Bytes 43..48 are unverified. Raise it with evidence, not lore.
MAX_P300_READ_LENGTH = 48


def validate_length_in(min_len: int, max_len: int):
    """Return a validator accepting an integer byte-length in [min_len, max_len]."""

    def validate(value):
        value = cv.positive_int(value)
        if not (min_len <= value <= max_len):
            raise cv.Invalid(f"length must be between {min_len} and {max_len} bytes (got {value})")
        return value

    return validate


def field_width(config) -> int:
    """The wire value width: ``byte_length`` under extraction, else ``length``.

    Shared by number/select/switch, which previously each carried a private
    ``_field_width`` with an identical body.
    """
    if CONF_BYTE_OFFSET in config:
        return config.get(CONF_BYTE_LENGTH, 1)
    return config[CONF_LENGTH]


def validate_block_extraction(plain_lengths: tuple):
    """Return the shared ``length`` / ``byte_offset`` / ``byte_length`` validator.

    number.py, select.py and switch.py all support aligned block extraction on
    the STATE read with byte-identical rules; only the set of widths allowed
    WITHOUT extraction differs (number accepts 1..4, select/switch 1..2), which
    is what ``plain_lengths`` parameterises.

    With ``byte_offset``, ``length`` is the block read at the state address and
    the field is ``byte_length`` (default 1) bytes at ``byte_offset``. The write
    still targets ``address`` -- the field's own register -- which is why
    ``byte_offset`` requires an explicit ``state_address``: writing field-width
    bytes at the block base would hit the wrong register.
    """

    def validate(config):
        length = config[CONF_LENGTH]
        if CONF_BYTE_OFFSET in config:
            if not 1 <= length <= MAX_P300_READ_LENGTH:
                raise cv.Invalid(
                    f"with byte_offset, length is a block read and must be 1..{MAX_P300_READ_LENGTH} (got {length})",
                    path=[CONF_LENGTH],
                )
            if CONF_STATE_ADDRESS not in config:
                raise cv.Invalid(
                    "byte_offset requires state_address: the aligned block is read at "
                    "state_address while address stays the field's own write register",
                    path=[CONF_BYTE_OFFSET],
                )
            width = config.get(CONF_BYTE_LENGTH, 1)
            if config[CONF_BYTE_OFFSET] + width > length:
                raise cv.Invalid(
                    f"byte_offset ({config[CONF_BYTE_OFFSET]}) + byte_length ({width}) must be <= length ({length})",
                    path=[CONF_BYTE_OFFSET],
                )
        else:
            if CONF_BYTE_LENGTH in config:
                raise cv.Invalid("byte_length requires byte_offset", path=[CONF_BYTE_LENGTH])
            if length not in plain_lengths:
                allowed = " or ".join(str(x) for x in plain_lengths)
                if len(plain_lengths) > 2:
                    raise cv.Invalid(f"length must be between 1 and 4 bytes (got {length})", path=[CONF_LENGTH])
                raise cv.Invalid(f"length must be {allowed} bytes (got {length})", path=[CONF_LENGTH])
        return config

    return validate


def validate_fault_codes(config, key):
    """Reject fault-code map keys that cannot fit the one-byte wire code.

    Shared by ``text_sensor`` (``type: error_history``) and ``event``: both map
    the decoded code BYTE to a label, so a wider key is a dead entry that can
    never match. Applied as a POST-validator, because a voluptuous key-marker
    that raises is swallowed into a generic 'extra keys' error -- the range check
    has to run after key parsing.
    """
    for code in config.get(key, {}):
        if not 0 <= code <= 0xFF:
            raise cv.Invalid(f"fault code 0x{code:X} does not fit one byte (0..0xFF)", path=[key])
    return config


def pop_poll_interval(config):
    """Remove and return the per-datapoint ``update_interval``, in milliseconds.

    MUST be called before ``cg.register_component``: ``update_interval`` is a
    reserved ESPHome key, and register_component would emit
    ``set_update_interval()`` -- a PollingComponent method these passive entities
    do not have. The hub drives polling; this is kept only as a per-datapoint
    poll interval, applied via :func:`emit_poll_interval`.
    """
    interval = config.pop(CONF_UPDATE_INTERVAL, None)
    return None if interval is None else int(interval.total_milliseconds)


def emit_poll_interval(var, poll_ms):
    """Emit ``set_poll_interval()`` when a per-datapoint interval was given.

    The hub schedules at hub-tick granularity and warns at runtime if this is
    shorter than the hub's own interval.
    """
    if poll_ms is not None:
        cg.add(var.set_poll_interval(poll_ms))


# --- hub-fed sensor tables (M4) ---------------------------------------------
#
# The hub's connectivity binary_sensors and its device_id / scan_result
# text_sensors are configured in their own platform blocks, so no single to_code
# ever sees the full set. Each platform therefore *collects* its entity here and
# a FINAL-priority step below emits one static table per kind and hands the hub
# a pointer and a count -- replacing three std::vectors that grew by push_back.
#
# The collection lives in CORE.data (not a module global) because CORE.reset()
# clears it: a module global would leak entities between configs compiled in one
# process, which is exactly what the test suite does.
#
# Single-hub assumption: the store is keyed only by sensor KIND, and a single
# _HUB_VAR records the one hub, so every collected sensor is attributed to it.
# This holds because MULTI_CONF is False -- exactly one `vitohome:` instance is
# permitted, hence one possible parent. If MULTI_CONF were ever set True (e.g.
# two Optolink buses on one node), this would misattribute every sensor to the
# last hub; the fix is to key the store by the resolved parent and emit one
# table per (hub, kind). The platform files still resolve CONF_VITOHOME_ID, so
# the parent handle is available to reintroduce.
_HUB_SENSORS = "hub_sensors"
_HUB_VAR = "hub_var"
HUB_LINK_SENSORS = "link"
HUB_DEVICE_ID_SENSORS = "device_id"
HUB_RAW_RESULT_SENSORS = "raw_result"


def register_hub_sensor(kind, var):
    """Collect a hub-fed sensor for the FINAL table emission."""
    store = CORE.data.setdefault("vitohome", {}).setdefault(_HUB_SENSORS, {})
    store.setdefault(kind, []).append(var)


def _emit_hub_sensor_table(hub, kind, sensors, cpp_type, setter):
    """Emit one `static T *const <hub>_<kind>_sensors[]` table and hand it to the hub.

    Deliberately NOT cg.static_const_array(): that renders `static const T name[]`,
    which for a pointer element type binds the const to the POINTEE
    (`const T *`). These sensors are published to, so the array must be
    `T *const` -- const pointers to non-const sensors -- to match the hub's
    `T *const *` member. Either way no heap is involved, which is the point.
    """
    if not sensors:
        return
    name = f"{hub}_{kind}_sensors"
    items = ", ".join(str(sensor) for sensor in sensors)
    cg.add(cg.RawStatement(f"static {cpp_type} *const {name}[] = {{{items}}};"))
    cg.add(getattr(hub, setter)(cg.RawExpression(name), len(sensors)))


@coroutine_with_priority(CoroPriority.FINAL)
async def _emit_hub_sensor_tables():
    """Emit the three hub-fed sensor tables once every platform has registered.

    Runs at FINAL priority so every entity variable is already declared by the
    time the table initializers reference them.
    """
    data = CORE.data.get("vitohome", {})
    hub = data.get(_HUB_VAR)
    store = data.get(_HUB_SENSORS, {})
    if hub is None or not store:
        return
    _emit_hub_sensor_table(
        hub, HUB_LINK_SENSORS, store.get(HUB_LINK_SENSORS), "binary_sensor::BinarySensor", "set_link_sensors"
    )
    _emit_hub_sensor_table(
        hub,
        HUB_DEVICE_ID_SENSORS,
        store.get(HUB_DEVICE_ID_SENSORS),
        "text_sensor::TextSensor",
        "set_device_id_sensors",
    )
    _emit_hub_sensor_table(
        hub,
        HUB_RAW_RESULT_SENSORS,
        store.get(HUB_RAW_RESULT_SENSORS),
        "text_sensor::TextSensor",
        "set_raw_result_sensors",
    )


def emit_option_table(config, mapping, suffix):
    """Emit ``static const VitoOption <id>_<suffix>[] = {...}`` and return (array, count).

    Replaces a run of one ``add_option()`` / ``add_code()`` statement per row.
    Each of those did an ``emplace_back`` on a growing ``std::vector``, so a
    94-entry fault map reallocated eight times (capacity 1 -> 128) and freed each
    predecessor, interleaved with every other allocation ESPHome makes during
    setup(). Over the complete catalog that is 1883 rows and 662 boot
    allocations -- absorbed on an ESP32, disqualifying on an ESP8266 (~40 KiB
    heap, umm_malloc, no MMU).

    The table lands in .rodata and the entity keeps only a pointer and a count,
    so the runtime cost is zero heap. Labels are escaped with
    :func:`cpp_string_literal` exactly as ``datapoint_expression`` does.

    Returns ``(None, 0)`` for an empty mapping: a zero-length array is not valid
    C++, and the entity's null/0 defaults already mean "no table".
    """
    if not mapping:
        return None, 0
    rows = [cg.RawExpression(f"{{{int(value)}, {cpp_string_literal(label)}}}") for value, label in mapping.items()]
    table_id = ID(f"{config[CONF_ID]}_{suffix}", is_declaration=True, type=VitoOption)
    return cg.static_const_array(table_id, cg.ArrayInitializer(*rows, multiline=True)), len(rows)


def emit_uint32_table(config, values, suffix):
    """Emit ``static const uint32_t <id>_<suffix>[] = {...}`` and return (array, count).

    The plain-value counterpart of :func:`emit_option_table`, for select's
    option values and switch's on-state values. Same rationale, same empty-input
    contract.
    """
    values = list(values)
    if not values:
        return None, 0
    table_id = ID(f"{config[CONF_ID]}_{suffix}", is_declaration=True, type=cg.uint32)
    return cg.static_const_array(table_id, cg.ArrayInitializer(*[int(v) for v in values])), len(values)


def emit_write_target(var, config, read_addr, write_addr):
    """Emit the write datapoint and any block-extraction setters.

    Shared by number/select/switch, whose to_code() bodies carried three
    byte-identical copies of this block.

    Under extraction the state read is ``length`` bytes at the state address
    while the WRITE datapoint carries only the FIELD width, so control() /
    write_state() writes exactly the field's bytes to the field's own register.
    Without extraction a write datapoint is emitted only when the read and write
    addresses actually differ.
    """
    if CONF_BYTE_OFFSET in config:
        width = config.get(CONF_BYTE_LENGTH, 1)
        cg.add(var.set_write_datapoint(datapoint_expression(config[CONF_NAME], write_addr, width)))
        cg.add(var.set_extract_byte(config[CONF_BYTE_OFFSET]))
        if CONF_BYTE_LENGTH in config:
            cg.add(var.set_extract_len(config[CONF_BYTE_LENGTH]))
    elif read_addr != write_addr:
        cg.add(var.set_write_datapoint(datapoint_expression(config[CONF_NAME], write_addr, config[CONF_LENGTH])))


def validate_converter_length(config):
    """Cross-check ``length`` against the converter's sensible length set.

    Surfaces at ``esphome config`` time rather than as a wrong decode at
    runtime.
    """
    name = config[CONF_CONVERTER]
    allowed = CONVERTERS[name].lengths
    if config[CONF_LENGTH] not in allowed:
        allowed_str = ", ".join(str(x) for x in allowed)
        raise cv.Invalid(
            f"converter '{name}' supports length {allowed_str} (got {config[CONF_LENGTH]})",
            path=[CONF_LENGTH],
        )
    return config


def resolve_signed(config) -> bool:
    """The explicit ``signed:`` if present, else the converter's default."""
    if config.get(CONF_SIGNED) is not None:
        return config[CONF_SIGNED]
    return CONVERTERS[config[CONF_CONVERTER]].default_signed


def raw_fits(raw_value: int, length: int, is_signed: bool) -> bool:
    """Whether ``raw_value`` fits ``length`` bytes (signed or unsigned).

    Mirrors the range check in ``decode.h::encode_scaled`` exactly, so the
    Python config-time check and the C++ runtime guard agree.
    """
    if is_signed:
        lo = -(1 << (8 * length - 1))
        hi = (1 << (8 * length - 1)) - 1
    else:
        lo = 0
        hi = (1 << (8 * length)) - 1
    return lo <= raw_value <= hi


def cpp_string_literal(value: str) -> str:
    """Return *value* as a safely-escaped C++ string literal (incl. quotes).

    Entity names are interpolated verbatim into generated C++ (the
    ``optolink::Datapoint`` name argument). A backslash or double-quote in the
    name would otherwise break the literal, so escape those characters.
    """
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def datapoint_expression(name: str, address: int, length: int) -> cg.RawExpression:
    """Build the ``optolink::Datapoint`` constructor expression.

    The converter slot is always ``noconv``: the component decodes/encodes the
    raw payload itself, so the engine converter is never used. ``name`` is
    escaped; ``address`` is emitted as a 0x-prefixed 16-bit literal. The type is
    fully qualified (``esphome::vitohome::optolink::``) because the expression is
    emitted into the global-scope generated ``main.cpp``.
    """
    return cg.RawExpression(
        f"esphome::vitohome::optolink::Datapoint("
        f"{cpp_string_literal(name)}, "
        f"{address:#06x}, "
        f"{length}, "
        f"esphome::vitohome::optolink::noconv"
        f")"
    )


def _validate_time_sync(config):
    """``time_sync`` needs a ``time_id`` to pull the current time from."""
    if CONF_TIME_SYNC in config and CONF_TIME_ID not in config:
        raise cv.Invalid(
            f"'{CONF_TIME_SYNC}' requires '{CONF_TIME_ID}' to select a time source (e.g. a homeassistant or sntp time:)",
            path=[CONF_TIME_SYNC],
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(VitoHomeComponent),
            cv.Optional(CONF_PROTOCOL, default="P300"): cv.enum(PROTOCOLS, upper=True),
            # Default depends on the protocol (on for P300, off for KW/GWG,
            # whose boot identification scheme differs); resolved in to_code.
            cv.Optional(CONF_IDENTIFY_DEVICE, visibility=cv.Visibility.ADVANCED): cv.boolean,
            # Log every Optolink telegram (>>> TX / <<< RX) under the
            # 'vitohome.frames' tag. Compile-time: off costs nothing.
            cv.Optional(CONF_LOG_FRAMES, default=False, visibility=cv.Visibility.ADVANCED): cv.boolean,
            # Optional system-time sync: write the device clock (default
            # 0x088E, overridable via time_sync.clock_address) from a time
            # source when it drifts. Inert unless time_id is set.
            cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            cv.Optional(CONF_TIME_SYNC): TIME_SYNC_SCHEMA,
            # Capacity of the interactive scan console's lane. Each slot costs
            # sizeof(RawOp) (~38 bytes on a 32-bit target), reserved once at
            # setup().
            #
            # DEFAULT 0: this lane serves one feature -- queue_raw_read/write
            # and the scan_result text_sensor -- and that feature is a debug
            # tool, so it is opt-in. Time sync does NOT use this lane (it is a
            # VitoClock entity on the read/write lanes), so there is no minimum
            # and no cross-check against time_id.
            #
            # Size it to the largest burst you intend: a one-off raw read from a
            # button needs 1; a RANGE SWEEP needs depth proportional to its
            # count (example/vitohome-scanner-raw.yaml uses 256). The required
            # depth cannot be derived here -- the lane is driven from lambdas,
            # and the shipped sweep's count is a Home Assistant action parameter
            # chosen at runtime. An enqueue against an unallocated lane is
            # rejected with a warning naming this option.
            cv.Optional(CONF_RAW_QUEUE_SIZE, default=0, visibility=cv.Visibility.ADVANCED): cv.int_range(min=0, max=1024),
        }
    )
    .extend(cv.polling_component_schema("60s"))
    .extend(uart.UART_DEVICE_SCHEMA),
    _validate_time_sync,
)


# ---------------------------------------------------------------------------
# Cross-platform final validation
#
# A platform schema cannot see which protocol its hub speaks. Two protocol-level
# constraints therefore have to be checked once the whole config is known.
_ADDRESS_DOMAINS = ("sensor", "binary_sensor", "text_sensor", "number", "select", "switch", "text", "event", "climate")

# GWG addresses a SINGLE BYTE. Source-confirmed in the vendored engine:
# PacketGWG::createPacket() REJECTS any address above 0xFF -- a guard
# inherited verbatim from upstream VitoWiFi @ edc059a7 (source-confirmed
# there too; an earlier comment here claimed upstream silently truncated,
# which was wrong). Corroborated by
# vcontrold, whose GWG device (ID 2053, "V200WB2 ID=2053 Protokoll:GWG_VBEM")
# overrides every command onto single-byte addresses (0x63, 0xF8, 0x22, 0x01,
# 0x17) rather than the 16-bit ones used on KW/P300.
# Consequence: a catalog generated for a P300/KW device -- 16-bit Vitosoft
# addresses -- is meaningless under `protocol: GWG`. (A catalog generated for a
# Vitosoft GWG_* device token is NOT: those tables are already 8-bit, 4066 of
# 4143 events across all 22 GWG tokens, and gen_catalog.py drops the rest. See
# its --protocol flag.) At runtime a rejected packet never leaves the hub's
# dispatch lane, so ONE such entity at the front of the read or write queue
# stalls that lane (and everything behind it) permanently.
# That failure mode is exactly why this must be a hard `esphome config` error.
# The check spans every platform that carries a 16-bit address: the flat
# address/state_address keys, climate's target_address, and climate's nested
# operating_mode block (handled in _entity_gwg_addresses below).
_ADDRESS_KEYS = ("address", "state_address", "target_address")

# climate: nests its Betriebsart command/state addresses one level down.
_CLIMATE_OPERATING_MODE = "operating_mode"
_NESTED_ADDRESS_KEYS = ("address", "state_address")


def _entity_gwg_addresses(entity):
    """Yield (key_path, address) for every Optolink address this entity config
    carries, including climate's nested operating_mode block."""
    for key in _ADDRESS_KEYS:
        addr = entity.get(key)
        if addr is not None:
            yield key, addr
    operating_mode = entity.get(_CLIMATE_OPERATING_MODE)
    if isinstance(operating_mode, dict):
        for key in _NESTED_ADDRESS_KEYS:
            addr = operating_mode.get(key)
            if addr is not None:
                yield f"{_CLIMATE_OPERATING_MODE}.{key}", addr


def _entity_gwg_access_slots(entity):
    """Yield (key_path, access_or_None) for every place this entity config COULD
    carry an access mode -- present or not.

    Mirrors _entity_gwg_addresses: climate is the one platform whose datapoints
    are not all at the top level, and its two channels (setpoint at
    target_address, Betriebsart at operating_mode.address) are separately
    addressed, so they carry separate access modes. The Betriebsart slot exists
    only when the optional operating_mode block does.

    Yielding absent slots too is what lets the "writable entity must state its
    mode" check below know which key to name."""
    yield CONF_ACCESS, entity.get(CONF_ACCESS)
    operating_mode = entity.get(_CLIMATE_OPERATING_MODE)
    if isinstance(operating_mode, dict):
        yield f"{_CLIMATE_OPERATING_MODE}.{CONF_ACCESS}", operating_mode.get(CONF_ACCESS)


def _entity_gwg_access_modes(entity):
    """The subset of _entity_gwg_access_slots that is actually set."""
    for key, access in _entity_gwg_access_slots(entity):
        if access is not None:
            yield key, access


def _entities_for_hub(full, domains, hub_id):
    for domain in domains:
        for entity in full.get(domain, []):
            if entity.get("platform") != "vitohome":
                continue
            if entity.get(CONF_VITOHOME_ID) not in (None, hub_id):
                continue  # targets a different hub
            yield domain, entity


# Entity-registering platform domains: each vitohome entry in one of these
# registers exactly one VitoEntityBase via register_entity(). (Connectivity
# binary_sensors and device_id/scan_result text_sensors early-return onto the
# link / device-id / raw-result lanes and never reach register_entity(); climate
# is handled separately below because it contributes one or two channels.)
_ENTITY_DOMAINS = ("sensor", "binary_sensor", "text_sensor", "number", "select", "switch", "text", "event")

# Internal, codegen-only key: the entity-registry capacity computed by
# _final_validate and consumed by to_code. Not a user-facing YAML option.
_CONF_ENTITY_CAPACITY = "_entity_capacity"


def _entity_capacity_upper_bound(full, hub_id, has_clock):
    """Upper bound on register_entity() calls for one hub, used to size entities_.

    Deliberately an UPPER bound, never exact: FixedVector::push_back silently
    drops pushes past capacity, so an undercount would lose entities, while an
    overcount only wastes a pointer or two. Sources of (safe) slack versus the
    runtime count: connectivity binary_sensors and device_id/scan_result
    text_sensors register on other lanes rather than as entities, and a climate
    without operating_mode has one channel, not the two assumed here.
    register_entity() also guards the dangerous direction -- it marks the
    component failed rather than dropping silently -- if this bound is ever low.
    """
    n = sum(1 for _domain, _entity in _entities_for_hub(full, _ENTITY_DOMAINS, hub_id))
    # Each climate contributes a setpoint channel plus at most one mode channel.
    n += 2 * sum(1 for _domain, _entity in _entities_for_hub(full, ("climate",), hub_id))
    if has_clock:
        n += 1  # VitoClock, registered in setup() under VITOHOME_TIME_SYNC
    return n


def _entity_name(entity):
    return entity.get(CONF_NAME, entity.get(CONF_ID, "<unnamed>"))


def _final_validate(config):
    protocol = str(config[CONF_PROTOCOL])
    full = fv.full_config.get()
    hub_id = config[CONF_ID]

    if protocol == "GWG":
        # System-time sync cannot work under GWG, at any address.
        #
        # GWG has no 8-byte DateTimeBCD clock datapoint at all: the Vitosoft
        # data models its clock as three separate 1-byte registers
        # (GWG_Uhrzeit_Wochentag~0x0074, _Stunde~0x0075, _Minute~0x0076), which
        # is a different read/write shape entirely -- not something
        # clock_address can point at. And the default 0x088E is over GWG's
        # 8-bit address space, so PacketGWG::createPacket() rejects it on every
        # single attempt.
        #
        # Without this check the feature is not broken-and-obvious, it is
        # broken-and-quiet: the config validates, the boot log prints a clock
        # dump_config block, and the only symptom is one engine warning per sync
        # interval -- hours apart, easily missed. Reject at config time instead.
        if CONF_TIME_ID in config:
            raise cv.Invalid(
                "system-time sync is not supported under the GWG protocol. GWG has no 8-byte "
                "date/time datapoint -- its clock is three separate 1-byte registers (weekday "
                "0x74 / hour 0x75 / minute 0x76), which this component does not implement -- and "
                "the default clock_address 0x088E is outside GWG's single-byte address space, so "
                "the engine would reject every request. Remove 'time_id' from the vitohome hub.",
                path=[CONF_TIME_ID],
            )

        for domain, entity in _entities_for_hub(full, _ADDRESS_DOMAINS, hub_id):
            for key, addr in _entity_gwg_addresses(entity):
                if addr > 0xFF:
                    raise cv.Invalid(
                        f"{domain} '{_entity_name(entity)}' uses {key} 0x{addr:04X}, but the GWG "
                        f"protocol addresses a single byte (0x00..0xFF) -- the engine rejects the "
                        f"request, which would permanently stall its dispatch lane at runtime. GWG "
                        f"uses its own 8-bit address space, so a catalog generated for a P300/KW "
                        f"device does not apply to it. Generate from a Vitosoft GWG_* device token "
                        f"instead (scripts/gen_catalog.py picks the GWG rules up from the token, or "
                        f"pass --protocol GWG)."
                    )

        # A writable GWG entity must STATE its access mode; the physical default
        # is never right for a write. Across all 22 Vitosoft GWG device tokens
        # (4143 events) the writable datapoints are EEPROM_WRITE (1082) and
        # BE_WRITE (280) -- and ZERO PHYSICAL_WRITE (see FCWRITE_TO_GWG_ACCESS
        # in scripts/gen_catalog.py, which counts them). So an unstated mode
        # silently emits 0xC8, a write telegram no documented GWG datapoint
        # uses, at an address whose meaning is mode-dependent -- i.e. a write to
        # a datapoint other than the intended one, with no error anywhere.
        #
        # This breaks no generated catalog: _is_writable() only calls a GWG
        # datapoint writable when its write mode equals its read mode, and a
        # blank FCRead resolves to "" which equals no write mode, so a
        # generated writable GWG entity always carries an explicit access:.
        # Only hand-written configs are affected.
        for domain, entity in _entities_for_hub(full, _WRITE_DOMAINS, hub_id):
            for key, access in _entity_gwg_access_slots(entity):
                if access is None:
                    raise cv.Invalid(
                        f"{domain} '{_entity_name(entity)}' writes but does not set '{key}'. "
                        f"Under GWG the access mode is half a datapoint's identity (the same "
                        f"address means different things in different modes), and it cannot be "
                        f"defaulted for a write: no Vitosoft GWG datapoint uses PHYSICAL_WRITE, "
                        f"so the implicit mode would emit a write telegram that matches no "
                        f"documented datapoint -- writing to something other than what you "
                        f"intended, silently. Set '{key}' explicitly (writable modes: physical, "
                        f"virtual, eeprom, xram, port, be; Vitosoft's writable GWG datapoints "
                        f"are eeprom or be), or declare this datapoint as a read-only sensor. "
                        f"scripts/gen_catalog.py emits the right value from Vitosoft's own "
                        f"FCRead/FCWrite fields.",
                        path=key.split("."),
                    )

        # A writable entity in an access mode that has no write telegram type
        # is a config error, not a runtime one. GWG's KMBUS modes are read-only
        # (the wiki gives no write TYPE byte for either), so GWGEngine::write()
        # refuses them permanently; the write lane drops a permanent refusal and
        # logs, which means the entity would look completely normal in Home
        # Assistant and just never reach the device. Catch it here, where the
        # message can name the option.
        for domain, entity in _entities_for_hub(full, _WRITE_DOMAINS, hub_id):
            for key, access in _entity_gwg_access_modes(entity):
                if str(access) in _READ_ONLY_ACCESS_MODES:
                    raise cv.Invalid(
                        f"{domain} '{_entity_name(entity)}' sets {key}: {access}, but that GWG "
                        f"access mode is read-only -- the protocol defines no write telegram type "
                        f"for it, so the engine would refuse every write and the entity would "
                        f"silently never reach the device. Use a writable access mode (physical, "
                        f"virtual, eeprom, xram, port, be), or declare this datapoint as a "
                        f"read-only sensor.",
                        path=key.split("."),
                    )

        # Read-only entities: WARN rather than reject. Unlike the write case
        # above, defaulting to physical here is defensible -- Vitosoft emits
        # rows with a blank FCRead, and gen_catalog.py deliberately maps blank
        # to physical ("no access row" -> the engine default), so a missing
        # mode is not necessarily a mistake. But it is still an assumption
        # being made silently about a datapoint's identity, and if it is wrong
        # the entity publishes a plausible-looking number from the wrong
        # register. Say so once per entity and let the person decide; erroring
        # would force `access: physical` onto every datapoint about which
        # Vitosoft itself says nothing, which trains people to paste a value
        # they have not thought about.
        _readonly_access_domains = tuple(d for d in _ACCESS_DOMAINS if d not in _WRITE_DOMAINS)
        for domain, entity in _entities_for_hub(full, _readonly_access_domains, hub_id):
            for key, access in _entity_gwg_access_slots(entity):
                if access is None:
                    _LOGGER.warning(
                        "%s '%s' does not set '%s'; assuming physical. Under GWG the same "
                        "address means different datapoints in different access modes (in "
                        "GWG_VBES_00, address 0x01 is five datapoints across four modes), so if "
                        "physical is not this datapoint's mode the entity will publish a "
                        "plausible-looking value read from the wrong register. "
                        "scripts/gen_catalog.py emits the right value from Vitosoft's own "
                        "FCRead field.",
                        domain,
                        _entity_name(entity),
                        key,
                    )
    else:
        # access: selects a GWG TYPE byte (GWGAccessMode, constants.h) and is
        # meaningless -- and silently a no-op at runtime, since VitoEntityBase::
        # access_ is compiled but never read outside a GWG build -- under
        # any other protocol. Reject at config time rather than let it look
        # configurable.
        for domain, entity in _entities_for_hub(full, _ACCESS_DOMAINS, hub_id):
            for key, _access in _entity_gwg_access_modes(entity):
                raise cv.Invalid(
                    f"{domain} '{_entity_name(entity)}' sets '{key}', which selects a GWG "
                    f"TYPE byte and has no effect under protocol '{protocol}'. Remove it "
                    f"or set protocol: GWG on the vitohome hub.",
                    path=key.split("."),
                )

    # There is deliberately NO P300 read-length check here. The warning that used
    # to live at this spot cited a 40-byte read at 0x7362 failing on hardware.
    # That address was fabricated by the catalog generator (block base +
    # BytePosition), and a 2-byte read at the same address fails identically --
    # so the observation says nothing about length. See MAX_P300_READ_LENGTH.

    # Codegen sizing (all protocols): a conservative upper bound on this hub's
    # register_entity() count, so entities_ can be a single-allocation
    # FixedVector. Computed here because the full config -- every platform's
    # entities -- is visible; read back in to_code(). This mutates the live
    # config node in place (final-validate return values are discarded).
    config[_CONF_ENTITY_CAPACITY] = _entity_capacity_upper_bound(full, hub_id, CONF_TIME_ID in config)


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    # The Optolink protocol engine is vendored in-tree under
    # ``components/vitohome/optolink/`` with two parallel, toolchain-specific
    # build descriptions: ``optolink/library.json`` (PlatformIO) and
    # ``optolink/CMakeLists.txt`` (ESP-IDF native toolchain). Both exist
    # because of the same underlying limitation, not because of anything
    # PlatformIO- or CMake-specific: ESPHome's own component file copier
    # (``loader.py``'s ``ComponentManifest.resources``) only copies files
    # sitting directly in a component's top level dir; it does not descend
    # into the engine's ``protocol/``, ``datapoint/`` and ``interface/``
    # subdirectories under EITHER toolchain. (ESPHome does have a one-level
    # recursive mode -- ``recursive_sources`` -- but it's hardcoded to
    # ESPHome's own ``esphome.core`` package, not available to external
    # components, and wouldn't reach two levels deep here regardless.) So the
    # engine has to be registered as its own separately-built unit no matter
    # which toolchain compiles it; only the registration mechanism differs:
    #
    #   esp32.toolchain: platformio (current default -- see design_notes.md
    #   SS11 for the toolchain default flip on ESPHome's dev channel): a
    #   ``file://`` library handed straight to PlatformIO's library
    #   dependency finder, which compiles the nested sources with their
    #   structure intact.
    #
    #   esp32.toolchain: esp-idf: PlatformIO is not involved at all, so a
    #   PlatformIO library declaration doesn't reach the build -- worse, if
    #   left registered unconditionally it actively breaks the build, because
    #   ESPHome's PlatformIO-library-to-IDF-component converter
    #   (esphome/platformio/library.py::convert_libraries, upstream) treats
    #   any non-empty ``repository`` string as a git remote with no
    #   ``file://`` case, and tries (and fails) to ``git clone`` this local
    #   directory. The ESP-IDF-native equivalent is
    #   ``esp32.add_idf_component(path=...)``, which writes a real ESP-IDF
    #   Component Manager ``path:`` dependency -- standard, documented IDF
    #   functionality, independent of ESPHome -- into the generated
    #   ``idf_component.yml``. See ``optolink/CMakeLists.txt`` for the
    #   ESP-IDF-side source list and its ``INCLUDE_DIRS ".."`` -- that's not
    #   parity with this function's ``-I`` flag below, it's a *replacement*
    #   for it; see the note there for why.
    #
    # NOTE: this ESP-IDF-native branch has been through full, successful
    # native compiles in this project's sandbox -- both esp32.framework.type:
    # esp-idf and : arduino, each ending in "Successfully compiled program."
    # with real firmware.factory.bin/firmware.ota.bin output, on the pinned
    # ESPHome version (2026.7.0) -- not just codegen succeeding. One real bug
    # was caught and fixed in the process: see optolink/CMakeLists.txt's
    # ``INCLUDE_DIRS ".."`` note. Reproducible with
    # ``esphome compile tests/test.esp32-idf-native.yaml``. Still never run
    # against real hardware, but esp32.toolchain: esp-idf is now the DEFAULT
    # (esp32/__init__.py defaults CONF_TOOLCHAIN to Toolchain.ESP_IDF), and the
    # CI ``compile-idf-native`` job exercises this exact path on every push --
    # so it is a live, gated path, not forward-proofing. Re-run that test config
    # after any change here or under optolink/ before relying on it again.
    #
    # The ``-I`` flag puts the component dir on the include path so the
    # component's ``#include "optolink/optolink.h"`` (and the engine's header
    # tree it pulls in) resolves against that same checkout location. This
    # only works under esp32.toolchain: platformio, though -- confirmed by a
    # real native-toolchain compile failing on exactly this include with the
    # flag left unconditional. ESPHome's native-toolchain generator
    # (``build_gen/espidf.py::get_project_cmakelists``, pinned version
    # 2026.7.0) filters ``CORE.build_flags`` down to flags starting with
    # ``-D``/``-W`` before propagating them project-wide; a plain ``-I`` flag
    # is silently dropped, and "main"'s own generated
    # ``idf_component_register()`` hardcodes ``INCLUDE_DIRS "." "esphome"``
    # with no extension point for a third-party component to add to. Under
    # the native toolchain, ``optolink/CMakeLists.txt`` exposing its own
    # parent dir via ``INCLUDE_DIRS ".."`` is what makes the same include
    # resolve instead -- "main" implicitly REQUIRES optolink via the ``path:``
    # dependency above, and ESP-IDF auto-propagates a required component's
    # public INCLUDE_DIRS to the requiring component. So the flag below is
    # platformio-only in practice even though it's added unconditionally;
    # it's simply inert (unused) under the native toolchain rather than
    # harmful, so there's no need to gate it the way the library
    # registration above has to be gated.
    component_dir = Path(__file__).resolve().parent
    optolink_dir = component_dir / "optolink"
    if CORE.using_toolchain_esp_idf:
        esp32.add_idf_component(name="optolink", path=str(optolink_dir))
    else:
        cg.add_library("optolink", None, f"file://{optolink_dir}")
    cg.add_build_flag(f"-I{component_dir}")

    # The VITOHOME_* gates below (protocol select, LOG_FRAMES, TIME_SYNC) are
    # emitted as global -D build flags, not via cg.add_define(). ESPHome's
    # contributor guide steers layout-affecting gates toward cg.add_define()
    # (developers.esphome.io/contributing/code) for two reasons: build-wide
    # consistency -- every TU sees the same #ifdef state, avoiding ODR
    # violations -- and tooling visibility, since the checked-in
    # esphome/core/defines.h declares every USE_* so clangd/IDEs can resolve
    # guarded paths. A global -D already delivers the first: it is applied to
    # every TU (platformio build_flags / native CMakeLists), so there is no
    # per-TU divergence and no ODR hazard. The second does NOT transfer to an
    # out-of-tree component -- it cannot add symbols to ESPHome's checked-in
    # defines.h -- so cg.add_define() would buy no extra IDE resolution here.
    # The build-flag form is kept deliberately; a considered deviation.

    # Build-time protocol selection: emit exactly one VITOHOME_PROTOCOL_* flag,
    # which selects the engine via protocol_select.h.
    #
    # NOTE: cv.enum returns the *key* the user typed (an EnumValue str), with the
    # mapped value only in .enum_value. Interpolating the key directly emitted
    # -DVITOHOME_PROTOCOL_VS1 for `protocol: VS1`, a flag protocol_select.h does
    # not recognise -- so the VS1/VS2 aliases silently built the default P300
    # engine. Normalise through PROTOCOLS so the flag is always one of
    # P300/KW/GWG, the tokens protocol_select.h's #if chain actually checks.
    protocol = PROTOCOLS[str(config[CONF_PROTOCOL])]
    cg.add_build_flag(f"-DVITOHOME_PROTOCOL_{protocol}")
    if protocol == "GWG":
        # KW graduated to hardware-confirmed (VScotHO1_72 field logs 2026-07:
        # reads, multi-byte clock writes after THIRD_PARTY.md #11, Schaltzeiten,
        # Betriebsart round-trips). GWG remains implemented + host-proven only.
        _LOGGER.warning(
            "vitohome protocol 'GWG' is selectable but UNTESTED on hardware; "
            "P300 and KW are the validated protocols. Please report results.",
        )

    if config[CONF_LOG_FRAMES]:
        # Frame logging lives in the UART adapter (vito_uart_interface.h), which
        # is where the request/response boundaries are actually known. A build
        # flag rather than a runtime setter, so a production build carries no
        # RX buffer and no per-byte branch.
        cg.add_build_flag("-DVITOHOME_LOG_FRAMES")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    # The hub-fed sensor tables (M4) can only be emitted once every platform has
    # registered its entities, so record the hub and schedule the FINAL step.
    # A single _HUB_VAR suffices because MULTI_CONF is False (one hub only); see
    # register_hub_sensor for what would change if multiple hubs were allowed.
    CORE.data.setdefault("vitohome", {})[_HUB_VAR] = var
    CORE.add_job(_emit_hub_sensor_tables)

    # Size entities_ once, ahead of every platform's register_entity() statement.
    # The hub's to_code runs before the platforms' (they DEPENDENCIES=["vitohome"]),
    # so this setter call is emitted first and runs first. _final_validate injected
    # the capacity (a safe upper bound); see _entity_capacity_upper_bound.
    cg.add(var.reserve_entities(config[_CONF_ENTITY_CAPACITY]))

    # Identification reads 0xF8..0xFB once at boot. Default ON for P300 and
    # KW: on KW the block read at 0x00F8 falls back to four length-1 reads,
    # and this is HARDWARE-CONFIRMED on VScotHO1_72 (0x20CB) over BOTH
    # protocols -- P300 log 2026-07-04 and KW log 2026-07-03 both dump
    # "Device: 0x20CB (VScotHO1) HW=0x03 SW=0x51". GWG stays default-off
    # (its single-byte scheme is untested on hardware); users can still opt
    # in explicitly there.
    identify = config.get(CONF_IDENTIFY_DEVICE)
    if identify is None:
        identify = protocol in ("P300", "KW")
    cg.add(var.set_identify_device(identify))

    # Raw-lane capacity is applied before setup() reserves the lane. Emitted
    # unconditionally so the generated main.cpp states the size it runs with.
    cg.add(var.set_raw_queue_capacity(config[CONF_RAW_QUEUE_SIZE]))

    # Optional system-time sync. The build flag compiles the now()-using paths
    # in the hub only when a time source is configured, so a build without
    # time_id pulls in no dependency on the time component.
    if CONF_TIME_ID in config:
        time_var = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time_source(time_var))
        cg.add_build_flag("-DVITOHOME_TIME_SYNC")
        sync = config.get(CONF_TIME_SYNC) or TIME_SYNC_SCHEMA({})
        # TIME_SYNC_SCHEMA({}) above supplies every default, clock_address
        # included, so the address is always explicit here even when the user
        # omitted the whole time_sync: block.
        cg.add(
            var.set_time_sync(
                int(sync[CONF_INTERVAL].total_milliseconds),
                int(sync[CONF_DRIFT_THRESHOLD].total_seconds),
                sync[CONF_SYNC_ON_BOOT],
                sync[CONF_CLOCK_ADDRESS],
            )
        )
