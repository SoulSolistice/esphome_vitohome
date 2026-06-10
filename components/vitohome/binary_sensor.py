import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import CONF_ADDRESS, CONF_NAME

from . import (
    CONF_VITOCONNECT_ID,
    VitoHomeComponent,
    cpp_string_literal,
    vitohome_ns,
)

DEPENDENCIES = ["vitohome"]

CONF_LENGTH = "length"
CONF_BYTE_OFFSET = "byte_offset"
CONF_BIT_MASK = "bit_mask"

VitoBinarySensor = vitohome_ns.class_("VitoBinarySensor", binary_sensor.BinarySensor, cg.Component)


def _validate_length(value):
    value = cv.positive_int(value)
    if value not in (1, 2, 4):
        raise cv.Invalid(f"length must be 1, 2, or 4 bytes (got {value})")
    return value


def _validate_offset_within_length(config):
    # Surface this at config time (`esphome config`) rather than from to_code,
    # which only runs during code generation / compile.
    if config[CONF_BYTE_OFFSET] >= config[CONF_LENGTH]:
        raise cv.Invalid(
            f"byte_offset ({config[CONF_BYTE_OFFSET]}) must be < "
            f"length ({config[CONF_LENGTH]})",
            path=[CONF_BYTE_OFFSET],
        )
    return config


CONFIG_SCHEMA = cv.All(
    binary_sensor.binary_sensor_schema(VitoBinarySensor)
    .extend(
        {
            cv.GenerateID(CONF_VITOCONNECT_ID): cv.use_id(VitoHomeComponent),
            cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
            cv.Optional(CONF_LENGTH, default=1): _validate_length,
            cv.Optional(CONF_BYTE_OFFSET, default=0): cv.int_range(min=0, max=3),
            cv.Optional(CONF_BIT_MASK, default=0xFF): cv.hex_uint8_t,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _validate_offset_within_length,
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VITOCONNECT_ID])
    var = await binary_sensor.new_binary_sensor(config)
    await cg.register_component(var, config)

    # Raw-bit read: the converter is irrelevant here (we index the payload
    # directly via byte_offset/bit_mask), so the datapoint always uses noconv.
    # Length still drives how many bytes are requested. Name is escaped.
    datapoint = cg.RawExpression(
        f"VitoWiFi::Datapoint("
        f"{cpp_string_literal(config[CONF_NAME])}, "
        f"{config[CONF_ADDRESS]:#06x}, "
        f"{config[CONF_LENGTH]}, "
        f"VitoWiFi::noconv"
        f")"
    )
    cg.add(var.set_datapoint(datapoint))
    cg.add(var.set_byte_offset(config[CONF_BYTE_OFFSET]))
    cg.add(var.set_bit_mask(config[CONF_BIT_MASK]))
    cg.add(parent.register_entity(var))
