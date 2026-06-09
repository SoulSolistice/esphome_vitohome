import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_NAME, CONF_ADDRESS

from . import (
    CONF_VITOCONNECT_ID,
    CONVERTERS,
    CONVERTER_LENGTHS,
    VitoHomeComponent,
    cpp_string_literal,
    vitohome_ns,
)

DEPENDENCIES = ["vitohome"]

CONF_LENGTH = "length"
CONF_CONVERTER = "converter"

VitoSensor = vitohome_ns.class_("VitoSensor", sensor.Sensor, cg.Component)


def _validate_length(value):
    value = cv.positive_int(value)
    if value not in (1, 2, 4):
        raise cv.Invalid(f"length must be 1, 2, or 4 bytes (got {value})")
    return value


def _validate_converter_length(config):
    # VitoWiFi's converters only accept specific payload lengths and enforce
    # that with assert(), which is compiled out under NDEBUG. Fail here, at
    # config time, so a mismatch surfaces on `esphome config` rather than as
    # silent wrong/zero data at runtime.
    allowed = CONVERTER_LENGTHS[config[CONF_CONVERTER]]
    if config[CONF_LENGTH] not in allowed:
        allowed_str = ", ".join(str(x) for x in allowed)
        raise cv.Invalid(
            f"converter '{config[CONF_CONVERTER]}' supports length "
            f"{allowed_str} (got {config[CONF_LENGTH]})",
            path=[CONF_LENGTH],
        )
    return config


CONFIG_SCHEMA = cv.All(
    sensor.sensor_schema(VitoSensor)
    .extend(
        {
            cv.GenerateID(CONF_VITOCONNECT_ID): cv.use_id(VitoHomeComponent),
            cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
            cv.Required(CONF_LENGTH): _validate_length,
            cv.Optional(CONF_CONVERTER, default="noconv"): cv.enum(
                CONVERTERS, lower=True
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _validate_converter_length,
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VITOCONNECT_ID])
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)

    # Construct the VitoWiFi::Datapoint inline: the name is the (escaped)
    # ESPHome entity name, and the converter is the raw C++ symbol mapped
    # from the YAML enum.
    datapoint = cg.RawExpression(
        f"VitoWiFi::Datapoint("
        f"{cpp_string_literal(config[CONF_NAME])}, "
        f"{config[CONF_ADDRESS]:#06x}, "
        f"{config[CONF_LENGTH]}, "
        f"{CONVERTERS[config[CONF_CONVERTER]]}"
        f")"
    )

    cg.add(var.set_datapoint(datapoint))
    cg.add(parent.register_entity(var))
