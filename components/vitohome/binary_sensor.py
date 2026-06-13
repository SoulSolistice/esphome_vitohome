import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import (
    CONF_ADDRESS,
    CONF_NAME,
    CONF_UPDATE_INTERVAL,
)

from . import (
    CONF_LENGTH,
    CONF_VITOCONNECT_ID,
    VitoHomeComponent,
    datapoint_expression,
    validate_length_in,
    vitohome_ns,
)

DEPENDENCIES = ["vitohome"]

CONF_BYTE_OFFSET = "byte_offset"
CONF_BIT_MASK = "bit_mask"

VitoBinarySensor = vitohome_ns.class_("VitoBinarySensor", binary_sensor.BinarySensor, cg.Component)


def _validate_offset_within_length(config):
    if config[CONF_BYTE_OFFSET] >= config[CONF_LENGTH]:
        raise cv.Invalid(
            f"byte_offset ({config[CONF_BYTE_OFFSET]}) must be < length ({config[CONF_LENGTH]})",
            path=[CONF_BYTE_OFFSET],
        )
    return config


CONFIG_SCHEMA = cv.All(
    binary_sensor.binary_sensor_schema(VitoBinarySensor)
    .extend(
        {
            cv.GenerateID(CONF_VITOCONNECT_ID): cv.use_id(VitoHomeComponent),
            cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
            cv.Optional(CONF_LENGTH, default=1): validate_length_in(1, 4),
            cv.Optional(CONF_BYTE_OFFSET, default=0): cv.int_range(min=0, max=3),
            cv.Optional(CONF_BIT_MASK, default=0xFF): cv.hex_uint8_t,
            cv.Optional(CONF_UPDATE_INTERVAL): cv.update_interval,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _validate_offset_within_length,
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VITOCONNECT_ID])
    var = await binary_sensor.new_binary_sensor(config)
    await cg.register_component(var, config)

    # Raw-bit read: index the payload directly via byte_offset/bit_mask, so the
    # datapoint converter is irrelevant (always noconv). Length still drives how
    # many bytes are requested.
    cg.add(
        var.set_datapoint(
            datapoint_expression(config[CONF_NAME], config[CONF_ADDRESS], config[CONF_LENGTH])
        )
    )
    cg.add(var.set_byte_offset(config[CONF_BYTE_OFFSET]))
    cg.add(var.set_bit_mask(config[CONF_BIT_MASK]))
    if CONF_UPDATE_INTERVAL in config:
        cg.add(var.set_poll_interval(int(config[CONF_UPDATE_INTERVAL].total_milliseconds)))

    cg.add(parent.register_entity(var))
