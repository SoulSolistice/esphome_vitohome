import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ADDRESS,
    CONF_NAME,
    CONF_UPDATE_INTERVAL,
)

from . import (
    CONF_CONVERTER,
    CONF_LENGTH,
    CONF_SIGNED,
    CONF_VITOCONNECT_ID,
    CONVERTERS,
    VitoHomeComponent,
    converter_scale,
    datapoint_expression,
    resolve_signed,
    validate_converter_length,
    validate_length_in,
    vitohome_ns,
)

DEPENDENCIES = ["vitohome"]

CONF_BYTE_OFFSET = "byte_offset"

VitoSensor = vitohome_ns.class_("VitoSensor", sensor.Sensor, cg.Component)


def _validate_byte_offset(config):
    # A single-byte extraction takes payload[byte_offset] (length-1 raw) and
    # applies scale/sign to that one byte; it replaces the old
    # ">> 8 & 0xFF" lambda for the PR2 pump-speed unit. The selected byte must
    # lie inside the requested payload.
    if CONF_BYTE_OFFSET not in config:
        return config
    if config[CONF_BYTE_OFFSET] >= config[CONF_LENGTH]:
        raise cv.Invalid(
            f"byte_offset ({config[CONF_BYTE_OFFSET]}) must be < length ({config[CONF_LENGTH]})",
            path=[CONF_BYTE_OFFSET],
        )
    return config


CONFIG_SCHEMA = cv.All(
    sensor.sensor_schema(VitoSensor)
    .extend(
        {
            cv.GenerateID(CONF_VITOCONNECT_ID): cv.use_id(VitoHomeComponent),
            cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
            cv.Required(CONF_LENGTH): validate_length_in(1, 4),
            cv.Optional(CONF_CONVERTER, default="noconv"): cv.enum(CONVERTERS, lower=True),
            cv.Optional(CONF_SIGNED): cv.boolean,
            cv.Optional(CONF_BYTE_OFFSET): cv.int_range(min=0, max=3),
            cv.Optional(CONF_UPDATE_INTERVAL): cv.update_interval,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    validate_converter_length,
    _validate_byte_offset,
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VITOCONNECT_ID])
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)

    cg.add(
        var.set_datapoint(
            datapoint_expression(config[CONF_NAME], config[CONF_ADDRESS], config[CONF_LENGTH])
        )
    )
    cg.add(var.set_scale(converter_scale(config[CONF_CONVERTER])))
    cg.add(var.set_signed(resolve_signed(config)))
    if CONF_BYTE_OFFSET in config:
        cg.add(var.set_extract_byte(config[CONF_BYTE_OFFSET]))
    if CONF_UPDATE_INTERVAL in config:
        # cv.update_interval yields a TimePeriod; the entity stores ms and the
        # hub schedules at hub-tick granularity (warns if shorter than the hub
        # interval).
        cg.add(var.set_poll_interval(int(config[CONF_UPDATE_INTERVAL].total_milliseconds)))

    cg.add(parent.register_entity(var))
