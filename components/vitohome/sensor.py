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
    MAX_P300_READ_LENGTH,
    VitoHomeComponent,
    converter_big_endian,
    datapoint_expression,
    resolve_signed,
    scale_literal,
    validate_converter_length,
    vitohome_ns,
)

DEPENDENCIES = ["vitohome"]

CONF_BYTE_OFFSET = "byte_offset"
CONF_BYTE_LENGTH = "byte_length"  # field width to extract at byte_offset (1..4)

VitoSensor = vitohome_ns.class_("VitoSensor", sensor.Sensor, cg.Component)


def _validate_byte_offset(config):
    # With byte_offset, `length` is the BLOCK read (bytes fetched from the
    # wire, read at the block base so P300 gets an aligned read); the decoded
    # value is the FIELD of byte_length bytes (default 1) starting at
    # byte_offset. The whole field must lie inside the fetched block, and the
    # block read must stay within the single-telegram cap so it does not NAK
    # on P300.
    if CONF_BYTE_LENGTH in config and CONF_BYTE_OFFSET not in config:
        raise cv.Invalid("byte_length requires byte_offset", path=[CONF_BYTE_LENGTH])
    if CONF_BYTE_OFFSET not in config:
        return config
    field_width = config.get(CONF_BYTE_LENGTH, 1)
    if config[CONF_BYTE_OFFSET] + field_width > config[CONF_LENGTH]:
        raise cv.Invalid(
            f"byte_offset ({config[CONF_BYTE_OFFSET]}) + byte_length ({field_width}) "
            f"must be <= length ({config[CONF_LENGTH]})",
            path=[CONF_BYTE_OFFSET],
        )
    return config


def _validate_length(config):
    # length is a plain field width (<=4, checked against the converter) UNLESS
    # byte_offset is set -- then it is a block read that may be as wide as the
    # P300 single-telegram cap so a byte deep inside a coding block is
    # reachable with an aligned read.
    length = config[CONF_LENGTH]
    if CONF_BYTE_OFFSET in config:
        if not 1 <= length <= MAX_P300_READ_LENGTH:
            raise cv.Invalid(
                f"with byte_offset, length is a block read and must be 1..{MAX_P300_READ_LENGTH} (got {length})",
                path=[CONF_LENGTH],
            )
    elif not 1 <= length <= 4:
        raise cv.Invalid(f"length must be between 1 and 4 bytes (got {length})", path=[CONF_LENGTH])
    return config


def _validate_converter_length_effective(config):
    # When extracting one byte, the converter decodes a length-1 value; check
    # the converter against 1, not the block read. Otherwise defer to the
    # normal converter-vs-length check.
    if CONF_BYTE_OFFSET in config:
        name = config[CONF_CONVERTER]
        allowed = CONVERTERS[name].lengths
        field_width = config.get(CONF_BYTE_LENGTH, 1)
        if field_width not in allowed:
            allowed_str = ", ".join(str(x) for x in allowed)
            raise cv.Invalid(
                f"converter '{name}' cannot decode a {field_width}-byte extracted field (supports length {allowed_str})",
                path=[CONF_CONVERTER],
            )
        return config
    return validate_converter_length(config)


CONFIG_SCHEMA = cv.All(
    sensor.sensor_schema(VitoSensor)
    .extend(
        {
            cv.GenerateID(CONF_VITOCONNECT_ID): cv.use_id(VitoHomeComponent),
            cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
            cv.Required(CONF_LENGTH): cv.positive_int,
            cv.Optional(CONF_CONVERTER, default="noconv"): cv.enum(CONVERTERS, lower=True),
            cv.Optional(CONF_SIGNED): cv.boolean,
            cv.Optional(CONF_BYTE_OFFSET): cv.int_range(min=0, max=MAX_P300_READ_LENGTH - 1),
            cv.Optional(CONF_BYTE_LENGTH): cv.int_range(min=1, max=4),
            cv.Optional(CONF_UPDATE_INTERVAL): cv.update_interval,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _validate_length,
    _validate_converter_length_effective,
    _validate_byte_offset,
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VITOCONNECT_ID])
    # Pop the reserved update_interval BEFORE register_component: it would
    # otherwise emit set_update_interval(), a PollingComponent method our
    # passive entities don't have. The hub drives polling; we keep this only as
    # a per-datapoint poll interval.
    poll_interval = config.pop(CONF_UPDATE_INTERVAL, None)
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)

    cg.add(var.set_datapoint(datapoint_expression(config[CONF_NAME], config[CONF_ADDRESS], config[CONF_LENGTH])))
    # scale_literal emits a C++ *double* literal; passing the Python float here
    # would emit `0.1f`, quantizing the scale to float32 before the double math.
    cg.add(var.set_scale(scale_literal(config[CONF_CONVERTER])))
    cg.add(var.set_big_endian(converter_big_endian(config[CONF_CONVERTER])))
    cg.add(var.set_signed(resolve_signed(config)))
    if CONF_BYTE_OFFSET in config:
        cg.add(var.set_extract_byte(config[CONF_BYTE_OFFSET]))
        if CONF_BYTE_LENGTH in config:
            cg.add(var.set_extract_len(config[CONF_BYTE_LENGTH]))
    if poll_interval is not None:
        # TimePeriod -> ms. The hub schedules at hub-tick granularity (and
        # warns at runtime if this is shorter than the hub interval).
        cg.add(var.set_poll_interval(int(poll_interval.total_milliseconds)))

    cg.add(parent.register_entity(var))
