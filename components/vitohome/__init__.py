"""ESPHome component for Viessmann Optolink (VitoWiFi-based).

Stage 1: P300 (VS2) protocol, read-only sensor and binary_sensor platforms,
bidirectional converters from VitoWiFi (used here for decode; encode wired
in Stage 2 alongside number/select platforms).
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID

CODEOWNERS = ["@yourhandle"]  # TODO: replace with your actual GitHub handle
DEPENDENCIES = ["uart"]
MULTI_CONF = False

CONF_VITOCONNECT_ID = "vitohome_id"
CONF_PROTOCOL = "protocol"

vitohome_ns = cg.esphome_ns.namespace("vitohome")
vito_wifi_ns = cg.global_ns.namespace("VitoWiFi")

VitoHomeComponent = vitohome_ns.class_(
    "VitoHomeComponent", cg.PollingComponent, uart.UARTDevice
)

# Stage 1 supports P300 (VS2) only. KW and GWG require separate template
# instantiations and different callback signatures; deferred to Stage 2.
PROTOCOLS = {
    "P300": "P300",
    "VS2": "P300",
}

# Bidirectional converters from VitoWiFi. These are used for decode in
# Stage 1; encode (write path) is wired in Stage 2.
# Add new converters here as needed; the value is the C++ symbol name.
CONVERTERS = {
    "noconv": "VitoWiFi::noconv",
    "div10":  "VitoWiFi::div10",
    "div2":   "VitoWiFi::div2",
}

# Valid payload lengths per converter, mirroring VitoWiFi's own constraints
# (Datapoint/Converter.cpp). VitoWiFi enforces these only with assert(), which
# is a no-op under NDEBUG (ESPHome release builds) -- so a mismatched length
# would silently decode wrong/zero values. The sensor platform cross-checks
# against this at config time instead. (div3600/length-4 is not exposed yet.)
CONVERTER_LENGTHS = {
    "noconv": (1, 2, 4),
    "div10": (1, 2),
    "div2": (1,),
}


def cpp_string_literal(value: str) -> str:
    """Return *value* as a safely-escaped C++ string literal (including quotes).

    Entity names are interpolated verbatim into generated C++ (the
    ``VitoWiFi::Datapoint`` name argument). A backslash or double-quote in the
    name would otherwise break the literal, so escape those characters.
    """
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(VitoHomeComponent),
            cv.Optional(CONF_PROTOCOL, default="P300"): cv.enum(PROTOCOLS, upper=True),
        }
    )
    .extend(cv.polling_component_schema("60s"))
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    # Pinned to an exact upstream commit for reproducible builds: no 4.x tag
    # with the generic-interface support exists yet, and a moving branch (#main)
    # would let an upstream commit silently change/break OTA for every device.
    # Bump this SHA deliberately, after re-validating against the new revision.
    cg.add_library(
        "VitoWiFi",
        None,
        "https://github.com/bertmelis/VitoWiFi.git#edc059a7c3df3de0a5de089ebc1bdbfc19ca6faa",
    )

    # CONF_PROTOCOL is validated (P300/VS2 only, both -> VS2) but intentionally
    # not consumed here: Stage 1 is hardwired to the VS2 template instantiation.
    # The key is reserved for Stage 2 (KW/GWG), which need different templates.
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
