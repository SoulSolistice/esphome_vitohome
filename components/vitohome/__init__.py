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

import logging
import math
from dataclasses import dataclass
from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import esp32, uart
from esphome.components import time as time_
from esphome.const import CONF_ID, CONF_INTERVAL, CONF_NAME, CONF_TIME_ID
from esphome.core import CORE

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@SoulSolistice"]
DEPENDENCIES = ["uart"]
# Every vito_*.cpp in this directory is always compiled, so each platform's
# base component must be available even when the user's config declares no
# entity of that type. AUTO_LOAD pulls the bases in (defining USE_<X> and
# copying their headers) so the component builds regardless of which
# platforms a given device config uses.
AUTO_LOAD = [
    "sensor",
    "binary_sensor",
    "text_sensor",
    "number",
    "select",
    "switch",
    "text",
    "climate",
    "event",
    "button",
]
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

# System-time sync options (hub-level; see VitoHomeComponent::set_time_sync).
CONF_TIME_SYNC = "time_sync"
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
    }
)

# Shared platform option names (used by sensor/number/select/text_sensor).
CONF_LENGTH = "length"
CONF_CONVERTER = "converter"
CONF_SIGNED = "signed"
CONF_READ_BACK = "read_back"

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


# Conservative single-telegram READ payload cap for P300/VS2.
#
# EVIDENCE STATUS: the exact value is NOT established. What is known:
#   * The openv "Protokoll 300" specification documents the telegram length as
#     a single byte ("Anzahl der Bytes zwischen dem Telegramm-Start-Byte (0x41)
#     und der Pruefsumme") and names NO maximum read length.
#     https://github.com/openv/openv/wiki/Protokoll-300
#   * vcontrold defines no read wider than 9 bytes anywhere in xml/300/vito.xml,
#     but that is a documented limitation of vcontrold itself, not of the
#     protocol -- see the openv wiki discussion of the "0..9 Byte Begrenzung".
#     So it corroborates nothing either way.
#   * Hardware, VScotHO1_72 (0x20CB), 2026-07-10, P300: a 22-byte read at
#     0x2500 succeeds; a 40-byte read at 0x7362 is answered with an error
#     telegram (MessageIdentifier 0x03). The same 40-byte read on KW returns 40
#     bytes of 0xFF.
#
# That 0xFF fill is exactly what an UNIMPLEMENTED address looks like on KW,
# which has no error channel. So the 0x7362 failure may be caused by the
# ADDRESS rather than the LENGTH, and the true cap -- if one exists -- is only
# bounded to [22, 39] by our own data. 37 is retained as a conservative gate
# for the generator and for byte_offset block reads; it is a HEURISTIC, not a
# measured constant. Do not cite it as fact.
#
# To settle it: read 0x7362 with length 2 on P300. Success => length is the
# cause. Error => the address is unsupported and this cap is fiction.
MAX_P300_READ_LENGTH = 37


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
_LENGTH_DOMAINS = ("sensor", "binary_sensor", "text_sensor", "number")
_ADDRESS_DOMAINS = ("sensor", "binary_sensor", "text_sensor", "number", "select", "switch", "text")

# GWG addresses a SINGLE BYTE. Source-confirmed in the vendored engine:
# PacketGWG::createPacket() serialises `_buffer[step++] = addr & 0xFF`, i.e. the
# high byte is silently discarded -- 0x2500 becomes 0x00. Corroborated by
# vcontrold, whose GWG device (ID 2053, "V200WB2 ID=2053 Protokoll:GWG_VBEM")
# overrides every command onto single-byte addresses (0x63, 0xF8, 0x22, 0x01,
# 0x17) rather than the 16-bit ones used on KW/P300.
# Consequence: the generated catalogs, which carry 16-bit Vitosoft addresses,
# are meaningless under `protocol: GWG`. Reading a truncated address is not an
# error the device can report -- it just answers the wrong datapoint. Hard fail.
_ADDRESS_KEYS = ("address", "state_address", "target_address")


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
        for domain, entity in _entities_for_hub(full, _ADDRESS_DOMAINS, hub_id):
            for key in _ADDRESS_KEYS:
                addr = entity.get(key)
                if addr is not None and addr > 0xFF:
                    raise cv.Invalid(
                        f"{domain} '{_entity_name(entity)}' uses {key} 0x{addr:04X}, but the GWG "
                        f"protocol addresses a single byte -- the engine sends `addr & 0xFF`, so this "
                        f"would silently read 0x{addr & 0xFF:02X} instead. GWG uses its own 8-bit "
                        f"address space; the generated catalogs (16-bit Vitosoft addresses) do not "
                        f"apply to it."
                    )

    if protocol == "P300":
        # A read wider than MAX_P300_READ_LENGTH has been observed to fail on
        # hardware (0x7362, 40 bytes -> error telegram), but the cause may be the
        # address rather than the length -- see the note on MAX_P300_READ_LENGTH.
        # Warn rather than reject: the openv protocol spec names no maximum, and
        # rejecting a config that would in fact work is the worse failure.
        for domain, entity in _entities_for_hub(full, _LENGTH_DOMAINS, hub_id):
            length = entity.get(CONF_LENGTH)
            if length is not None and length > MAX_P300_READ_LENGTH:
                _LOGGER.warning(
                    "%s '%s' reads %d bytes. On P300 a %d-byte read at 0x7362 was answered with an "
                    "error telegram on hardware (VScotHO1_72). The protocol spec names no maximum, so "
                    "this may work on your device -- but if the entity stays unavailable with a "
                    "'protocol error', shorten it to <= %d bytes or use `protocol: KW`.",
                    domain,
                    _entity_name(entity),
                    length,
                    length,
                    MAX_P300_READ_LENGTH,
                )


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

    # Optional system-time sync. The build flag compiles the now()-using paths
    # in the hub only when a time source is configured, so a build without
    # time_id pulls in no dependency on the time component.
    if CONF_TIME_ID in config:
        time_var = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time_source(time_var))
        cg.add_build_flag("-DVITOHOME_TIME_SYNC")
        sync = config.get(CONF_TIME_SYNC) or TIME_SYNC_SCHEMA({})
        cg.add(
            var.set_time_sync(
                int(sync[CONF_INTERVAL].total_milliseconds),
                int(sync[CONF_DRIFT_THRESHOLD].total_seconds),
                sync[CONF_SYNC_ON_BOOT],
            )
        )
