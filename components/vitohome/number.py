import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import CONF_ADDRESS, CONF_MAX_VALUE, CONF_MIN_VALUE, CONF_NAME, CONF_STEP, CONF_UPDATE_INTERVAL

from . import (
    CONF_BYTE_LENGTH,
    CONF_BYTE_OFFSET,
    CONF_CONVERTER,
    CONF_LENGTH,
    CONF_READ_BACK,
    CONF_SIGNED,
    CONF_STATE_ADDRESS,
    CONF_VITOHOME_ID,
    CONVERTERS,
    MAX_P300_READ_LENGTH,
    VitoHomeComponent,
    converter_scale,
    datapoint_expression,
    emit_poll_interval,
    emit_write_target,
    field_width,
    llround,
    pop_poll_interval,
    raw_fits,
    resolve_signed,
    scale_literal,
    validate_block_extraction,
    vitohome_ns,
)

DEPENDENCIES = ["vitohome"]

VitoNumber = vitohome_ns.class_("VitoNumber", number.Number, cg.Component)

# Optional read/state address, distinct from the command (write) CONF_ADDRESS,
# plus aligned block extraction on the STATE read -- the same read/write split
# select.py / switch.py support. The rules are identical across the three and
# live in validate_block_extraction() / field_width() / emit_write_target() in
# __init__.py; number is the one that also allows 3- and 4-byte plain widths.


def _validate_converter_field_width(config):
    """Check the converter against the FIELD width (the extracted byte_length
    with byte_offset, else length) -- mirrors sensor.py's effective check."""
    name = config[CONF_CONVERTER]
    allowed = CONVERTERS[name].lengths
    width = field_width(config)
    if width not in allowed:
        allowed_str = ", ".join(str(x) for x in allowed)
        raise cv.Invalid(
            f"converter '{name}' cannot encode/decode a {width}-byte field (supports length {allowed_str})",
            path=[CONF_CONVERTER],
        )
    return config


# Only converters that have a defined inverse may back a writable number.
_WRITABLE_CONVERTERS = {k: v for k, v in CONVERTERS.items() if v.encodable}


def _validate_encodable_range(config):
    """Reject min/max that cannot be represented on the wire.

    This is the load-bearing config-time check for the write path: it mirrors
    ``decode.h::encode_scaled`` exactly (round the raw step half away from zero
    -- ``llround``, NOT Python's half-to-even ``round()``, which diverges at
    negative half-steps -- then range-check for the byte width and sign), so an
    un-encodable bound is an ``esphome config`` error rather than a runtime
    "value not written" log.
    """
    scale = converter_scale(config[CONF_CONVERTER])
    is_signed = resolve_signed(config)
    width = field_width(config)
    for key in (CONF_MIN_VALUE, CONF_MAX_VALUE):
        raw = llround(config[key] / scale)
        if not raw_fits(raw, width, is_signed):
            kind = "signed" if is_signed else "unsigned"
            raise cv.Invalid(
                f"{key} ({config[key]}) -> raw {raw} does not fit {width} "
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
            cv.GenerateID(CONF_VITOHOME_ID): cv.use_id(VitoHomeComponent),
            cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
            cv.Optional(CONF_STATE_ADDRESS): cv.hex_uint16_t,
            cv.Optional(CONF_LENGTH, default=1): cv.positive_int,
            cv.Optional(CONF_BYTE_OFFSET): cv.int_range(min=0, max=MAX_P300_READ_LENGTH - 1),
            cv.Optional(CONF_BYTE_LENGTH): cv.int_range(min=1, max=4),
            cv.Optional(CONF_CONVERTER, default="noconv"): cv.enum(_WRITABLE_CONVERTERS, lower=True),
            cv.Optional(CONF_SIGNED): cv.boolean,
            cv.Required(CONF_MIN_VALUE): cv.float_,
            cv.Required(CONF_MAX_VALUE): cv.float_,
            cv.Required(CONF_STEP): cv.positive_float,
            cv.Optional(CONF_READ_BACK, default=True): cv.boolean,
            cv.Optional(CONF_UPDATE_INTERVAL): cv.update_interval,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    validate_block_extraction((1, 2, 3, 4)),
    _validate_converter_field_width,
    _validate_encodable_range,
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VITOHOME_ID])
    poll_ms = pop_poll_interval(config)
    var = await number.new_number(
        config,
        min_value=config[CONF_MIN_VALUE],
        max_value=config[CONF_MAX_VALUE],
        step=config[CONF_STEP],
    )
    await cg.register_component(var, config)

    # CONF_ADDRESS is the command (write) address; with CONF_STATE_ADDRESS the
    # live value is read there instead -- identical to select.py / switch.py.
    write_addr = config[CONF_ADDRESS]
    read_addr = config.get(CONF_STATE_ADDRESS, write_addr)
    cg.add(var.set_datapoint(datapoint_expression(config[CONF_NAME], read_addr, config[CONF_LENGTH])))
    emit_write_target(var, config, read_addr, write_addr)
    # scale_literal emits a C++ *double* literal (see sensor.py / __init__.py).
    cg.add(var.set_scale(scale_literal(config[CONF_CONVERTER])))
    cg.add(var.set_signed(resolve_signed(config)))
    cg.add(var.set_read_back(config[CONF_READ_BACK]))
    emit_poll_interval(var, poll_ms)

    cg.add(parent.register_entity(var))
