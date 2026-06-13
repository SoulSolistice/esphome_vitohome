import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import (
    CONF_ADDRESS,
    CONF_MAX_VALUE,
    CONF_MIN_VALUE,
    CONF_NAME,
    CONF_STEP,
    CONF_UPDATE_INTERVAL,
)

from . import (
    CONF_CONVERTER,
    CONF_LENGTH,
    CONF_READ_BACK,
    CONF_SIGNED,
    CONF_VITOCONNECT_ID,
    CONVERTERS,
    VitoHomeComponent,
    converter_scale,
    datapoint_expression,
    raw_fits,
    resolve_signed,
    validate_converter_length,
    validate_length_in,
    vitohome_ns,
)

DEPENDENCIES = ["vitohome"]

VitoNumber = vitohome_ns.class_("VitoNumber", number.Number, cg.Component)

# Only converters that have a defined inverse may back a writable number.
_WRITABLE_CONVERTERS = {k: v for k, v in CONVERTERS.items() if v.encodable}


def _validate_encodable_range(config):
    """Reject min/max that cannot be represented on the wire.

    This is the load-bearing config-time check for the write path: it mirrors
    ``decode.h::encode_scaled`` exactly (round to nearest raw step, then range
    check for the byte width and sign), so an un-encodable bound is an
    ``esphome config`` error rather than a runtime "value not written" log.
    """
    scale = converter_scale(config[CONF_CONVERTER])
    is_signed = resolve_signed(config)
    length = config[CONF_LENGTH]
    for key in (CONF_MIN_VALUE, CONF_MAX_VALUE):
        raw = round(config[key] / scale)
        if not raw_fits(raw, length, is_signed):
            kind = "signed" if is_signed else "unsigned"
            raise cv.Invalid(
                f"{key} ({config[key]}) -> raw {raw} does not fit {length} "
                f"{kind} byte(s) with converter '{config[CONF_CONVERTER]}' "
                f"(scale {scale}). Pick a different length/converter or bound.",
                path=[key],
            )
    if config[CONF_MIN_VALUE] > config[CONF_MAX_VALUE]:
        raise cv.Invalid("min_value must be <= max_value", path=[CONF_MIN_VALUE])
    return config


CONFIG_SCHEMA = cv.All(
    number.number_schema(VitoNumber)
    .extend(
        {
            cv.GenerateID(CONF_VITOCONNECT_ID): cv.use_id(VitoHomeComponent),
            cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
            cv.Optional(CONF_LENGTH, default=1): validate_length_in(1, 4),
            cv.Optional(CONF_CONVERTER, default="noconv"): cv.enum(
                _WRITABLE_CONVERTERS, lower=True
            ),
            cv.Optional(CONF_SIGNED): cv.boolean,
            cv.Required(CONF_MIN_VALUE): cv.float_,
            cv.Required(CONF_MAX_VALUE): cv.float_,
            cv.Required(CONF_STEP): cv.positive_float,
            cv.Optional(CONF_READ_BACK, default=True): cv.boolean,
            cv.Optional(CONF_UPDATE_INTERVAL): cv.update_interval,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    validate_converter_length,
    _validate_encodable_range,
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VITOCONNECT_ID])
    var = await number.new_number(
        config,
        min_value=config[CONF_MIN_VALUE],
        max_value=config[CONF_MAX_VALUE],
        step=config[CONF_STEP],
    )
    await cg.register_component(var, config)

    cg.add(
        var.set_datapoint(
            datapoint_expression(
                config[CONF_NAME], config[CONF_ADDRESS], config[CONF_LENGTH]
            )
        )
    )
    cg.add(var.set_scale(converter_scale(config[CONF_CONVERTER])))
    cg.add(var.set_signed(resolve_signed(config)))
    cg.add(var.set_read_back(config[CONF_READ_BACK]))
    if CONF_UPDATE_INTERVAL in config:
        cg.add(var.set_poll_interval(int(config[CONF_UPDATE_INTERVAL].total_milliseconds)))

    cg.add(parent.register_entity(var))
