"""ESPHome component for Viessmann Optolink (VitoWiFi-based).

Stage 2: P300 (VS2) protocol with sensor, binary_sensor, text_sensor, number
and select platforms. The component decodes and encodes raw Optolink payloads
itself (see ``decode.h``) and uses VitoWiFi only as the wire/transport engine;
the VitoWiFi converters are never exercised (every ``Datapoint`` is built with
``VitoWiFi::noconv`` and the raw-bytes write overload is used).

Why decode in-component rather than via VitoWiFi's converters:
  * ``VitoWiFi::VariantValue`` is a non-discriminated union, so reading the
    wrong member silently returns garbage; and
  * VitoWiFi does all converter math in float32, which loses precision for
    4-byte counters (uint32 -> float drops bits above 2**24).
``decode.h`` extracts the integer in int64/uint64, scales in double, and only
narrows the *final* value to the float32 ESPHome state requires.
"""

from dataclasses import dataclass

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID

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
vito_wifi_ns = cg.global_ns.namespace("VitoWiFi")

VitoHomeComponent = vitohome_ns.class_("VitoHomeComponent", cg.PollingComponent, uart.UARTDevice)

# Stage 2 supports P300 (VS2) only. KW (VS1) and GWG are separate protocols
# with different framing and callback shapes; deferred to a later stage.
PROTOCOLS = {
    "P300": "P300",
    "VS2": "P300",
}

# VitoWiFi commit pin. Pinned to an exact upstream commit for reproducible
# builds: no 4.x tag with the generic-interface support exists yet, and a
# moving branch (#main) would let an upstream commit silently change/break OTA
# for every device. Bump this SHA deliberately, after re-validating the C++
# API surface against the new revision (the VitoWiFi facts in
# docs/stage2_design.md cite this exact commit).
VITOWIFI_REPO = "https://github.com/bertmelis/VitoWiFi.git"
VITOWIFI_COMMIT = "edc059a7"


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
    ``VitoWiFi::Datapoint`` name argument). A backslash or double-quote in the
    name would otherwise break the literal, so escape those characters.
    """
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def datapoint_expression(name: str, address: int, length: int) -> cg.RawExpression:
    """Build the ``VitoWiFi::Datapoint`` constructor expression.

    The converter slot is always ``noconv``: the component decodes/encodes the
    raw payload itself, so the library converter is never used. ``name`` is
    escaped; ``address`` is emitted as a 0x-prefixed 16-bit literal.
    """
    return cg.RawExpression(
        f"VitoWiFi::Datapoint("
        f"{cpp_string_literal(name)}, "
        f"{address:#06x}, "
        f"{length}, "
        f"VitoWiFi::noconv"
        f")"
    )


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(VitoHomeComponent),
            cv.Optional(CONF_PROTOCOL, default="P300"): cv.enum(PROTOCOLS, upper=True),
            cv.Optional(CONF_IDENTIFY_DEVICE, default=True): cv.boolean,
        }
    )
    .extend(cv.polling_component_schema("60s"))
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    cg.add_library(
        name="VitoWiFi",
        version=None,
        repository=f"{VITOWIFI_REPO}#{VITOWIFI_COMMIT}",
    )

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_identify_device(config[CONF_IDENTIFY_DEVICE]))
