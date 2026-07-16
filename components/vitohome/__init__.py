"""ESPHome component for Viessmann Optolink (vendored optolink engine).

P300 (VS2) is the validated protocol; KW (VS1) and GWG are build-time selectable
through the same adapter but untested. Platforms: sensor, binary_sensor,
text_sensor, number and select. The component decodes and encodes raw Optolink
payloads itself (see ``decode.h``) and uses the in-tree optolink engine (under
``optolink/``) only as the wire/transport layer; the engine's converters are
never exercised (every ``Datapoint`` is built with ``optolink::noconv`` and the
raw-bytes write overload is used).

Why decode in-component rather than via the engine's converters:
  * ``optolink::VariantValue`` is a non-discriminated union, so reading the
    wrong member silently returns garbage; and
  * the engine does all converter math in float32, which loses precision for
    4-byte counters (uint32 -> float drops bits above 2**24).
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
from esphome.const import CONF_ID, CONF_INTERVAL, CONF_NAME, CONF_TIME_ID
from esphome.core import CORE
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

CONF_VITOCONNECT_ID = "vitohome_id"
CONF_PROTOCOL = "protocol"
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
CONF_LENGTH = "length"
CONF_CONVERTER = "converter"
CONF_SIGNED = "signed"
CONF_READ_BACK = "read_back"
CONF_STATE_ADDRESS = "state_address"
CONF_BYTE_OFFSET = "byte_offset"
CONF_BYTE_LENGTH = "byte_length"

vitohome_ns = cg.esphome_ns.namespace("vitohome")

VitoHomeComponent = vitohome_ns.class_("VitoHomeComponent", cg.PollingComponent, uart.UARTDevice)

# Selectable protocols. P300 (VS2) is the only one exercised on hardware; KW
# (VS1) and GWG are wired through the same adapter but untested -- selecting
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

    Note (vs. Stage 1): because the component now decodes the payload itself,
    these length sets are about what is *physically sensible and float32-safe
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


def converter_lengths(name: str) -> tuple:
    return CONVERTERS[name].lengths


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
            cv.Optional(CONF_IDENTIFY_DEVICE): cv.boolean,
            # Log every Optolink telegram (>>> TX / <<< RX) under the
            # 'vitohome.frames' tag. Compile-time: off costs nothing.
            cv.Optional(CONF_LOG_FRAMES, default=False): cv.boolean,
            # Optional system-time sync: write the device clock (0x088E) from a
            # time source when it drifts. Inert unless time_id is set.
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
            cv.Optional(CONF_RAW_QUEUE_SIZE, default=0): cv.int_range(min=0, max=1024),
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
# Consequence: the generated catalogs, which carry 16-bit Vitosoft addresses,
# are meaningless under `protocol: GWG` -- and at runtime a rejected packet
# never leaves the hub's dispatch lane, so ONE such entity at the front of the
# read or write queue stalls that lane (and everything behind it) permanently.
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


def _entities_for_hub(full, domains, hub_id):
    for domain in domains:
        for entity in full.get(domain, []):
            if entity.get("platform") != "vitohome":
                continue
            if entity.get(CONF_VITOCONNECT_ID) not in (None, hub_id):
                continue  # targets a different hub
            yield domain, entity


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
                        f"uses its own 8-bit address space; the generated catalogs (16-bit Vitosoft "
                        f"addresses) do not apply to it."
                    )

    # There is deliberately NO P300 read-length check here. The warning that used
    # to live at this spot cited a 40-byte read at 0x7362 failing on hardware.
    # That address was fabricated by the catalog generator (block base +
    # BytePosition), and a 2-byte read at the same address fails identically --
    # so the observation says nothing about length. See MAX_P300_READ_LENGTH.


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
    # ESPHome version (2026.6.2) -- not just codegen succeeding. One real bug
    # was caught and fixed in the process: see optolink/CMakeLists.txt's
    # ``INCLUDE_DIRS ".."`` note. Reproducible with
    # ``esphome compile tests/test.esp32-idf-native.yaml``. Still never run
    # against real hardware, and nothing selects esp32.toolchain: esp-idf
    # today -- this is forward-proofing for if/when that becomes the default,
    # not a currently-exercised path. Re-run that test config after any
    # change here or under optolink/ before relying on it again.
    #
    # The ``-I`` flag puts the component dir on the include path so the
    # component's ``#include "optolink/optolink.h"`` (and the engine's header
    # tree it pulls in) resolves against that same checkout location. This
    # only works under esp32.toolchain: platformio, though -- confirmed by a
    # real native-toolchain compile failing on exactly this include with the
    # flag left unconditional. ESPHome's native-toolchain generator
    # (``build_gen/espidf.py::get_project_cmakelists``, pinned version
    # 2026.6.2) filters ``CORE.build_flags`` down to flags starting with
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

    # Build-time protocol selection: emit exactly one VITOHOME_PROTOCOL_* flag,
    # which selects the engine via protocol_select.h.
    #
    # NOTE: cv.enum returns the *key* the user typed (an EnumValue str), with the
    # mapped value only in .enum_value. Interpolating the key directly emitted
    # -DVITOHOME_PROTOCOL_VS1 for `protocol: VS1`, a flag protocol_select.h does
    # not recognise -- so the VS1/VS2 aliases silently built the default P300
    # engine. Normalise through PROTOCOLS so the flag is always one of
    # P300/KW/GWG, the tokens the adapter's #if chain actually checks.
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
