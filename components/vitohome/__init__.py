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
from dataclasses import dataclass
from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@yourhandle"]  # TODO: replace with your actual GitHub handle
DEPENDENCIES = ["uart"]
MULTI_CONF = False

CONF_VITOCONNECT_ID = "vitohome_id"
CONF_PROTOCOL = "protocol"
CONF_IDENTIFY_DEVICE = "identify_device"

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
# (P300/KW/GWG) that selects the engine inside ProtocolAdapter.
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
}


def converter_scale(name: str) -> float:
    return CONVERTERS[name].scale


def converter_default_signed(name: str) -> bool:
    return CONVERTERS[name].default_signed


def converter_lengths(name: str) -> tuple:
    return CONVERTERS[name].lengths


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


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(VitoHomeComponent),
            cv.Optional(CONF_PROTOCOL, default="P300"): cv.enum(PROTOCOLS, upper=True),
            # Default depends on the protocol (on for P300, off for KW/GWG,
            # whose boot identification scheme differs); resolved in to_code.
            cv.Optional(CONF_IDENTIFY_DEVICE): cv.boolean,
        }
    )
    .extend(cv.polling_component_schema("60s"))
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    # The Optolink protocol engine is vendored in-tree under
    # ``components/vitohome/optolink/`` and compiled as a PlatformIO library via
    # its own ``optolink/library.json`` manifest.
    #
    # We register it from the component's own directory (``__file__`` -> the
    # clone/checkout location, where the full nested tree exists) rather than
    # relying on ESPHome's component file copier: that copier only copies files
    # sitting directly in the component dir and does NOT descend into nested
    # subdirectories, so the engine's ``protocol/``, ``datapoint/`` and
    # ``interface/`` trees would never reach the build. A ``file://`` library
    # is handed straight to PlatformIO's library dependency finder, which
    # compiles the nested sources with their structure intact and works for a
    # remotely-pulled component (``git clone`` brings the whole subtree).
    #
    # The ``-I`` flag puts the component dir on the include path so the
    # component's ``#include "optolink/optolink.h"`` (and the engine's header
    # tree it pulls in) resolves against that same checkout location.
    component_dir = Path(__file__).resolve().parent
    optolink_dir = component_dir / "optolink"
    cg.add_library("optolink", None, f"file://{optolink_dir}")
    cg.add_build_flag(f"-I{component_dir}")

    # Build-time protocol selection: emit exactly one VITOHOME_PROTOCOL_* flag,
    # which selects the engine inside ProtocolAdapter.
    protocol = config[CONF_PROTOCOL]
    cg.add_build_flag(f"-DVITOHOME_PROTOCOL_{protocol}")
    if protocol != "P300":
        _LOGGER.warning(
            "vitohome protocol '%s' is selectable but UNTESTED on hardware; "
            "P300 (VS2) is the validated protocol. Please report results.",
            protocol,
        )

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    # Identification (0xF8..0xFB) is a P300-era scheme; default it off for
    # KW/GWG unless the user explicitly enables it.
    identify = config.get(CONF_IDENTIFY_DEVICE)
    if identify is None:
        identify = protocol == "P300"
    cg.add(var.set_identify_device(identify))
