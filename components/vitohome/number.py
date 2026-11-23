import logging

import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import CONF_ADDRESS, CONF_MAX_VALUE, CONF_MIN_VALUE, CONF_NAME, CONF_STEP, CONF_UPDATE_INTERVAL

from . import (
    BIT_MASK_SCHEMA,
    CONF_ACCESS,
    CONF_BIT_MASK,
    CONF_BYTE_LENGTH,
    CONF_BYTE_OFFSET,
    CONF_CONVERTER,
    CONF_LENGTH,
    CONF_READ_BACK,
    CONF_SIGNED,
    CONF_STATE_ADDRESS,
    CONF_VITOHOME_ID,
    CONVERTERS,
    GWG_ACCESS_MODES,
    MAX_P300_READ_LENGTH,
    VitoHomeComponent,
    bit_field_max,
    converter_scale,
    datapoint_expression,
    emit_bit_mask,
    emit_poll_interval,
    emit_write_target,
    field_width,
    llround,
    poll_update_interval,
    pop_poll_interval,
    raw_fits,
    resolve_signed,
    scale_literal,
    validate_bit_field,
    validate_block_extraction,
    vitohome_ns,
)

DEPENDENCIES = ["vitohome"]

_LOGGER = logging.getLogger(__name__)

# value = raw * scale + value_offset, for a datapoint whose zero is not raw 0
# (a 4-bit coding field 0..15 that means 35..110 °C: mult5, value_offset 35).
CONF_VALUE_OFFSET = "value_offset"
# A scale no converter preset carries (Vitosoft MultOffset factor 3, 1/64, ...).
CONF_SCALE = "scale"
# Tolerance for "a whole number of raw steps"; the scale is a decimal, not exact.
_GRID_EPS = 1e-6

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


def _effective_scale(config) -> float:
    return config.get(CONF_SCALE, converter_scale(config[CONF_CONVERTER]))


def _validate_scale(config):
    """`scale` replaces a converter preset, it does not stack on one."""
    if CONF_SCALE in config and config[CONF_CONVERTER] != "noconv":
        raise cv.Invalid(
            f"scale cannot be combined with converter '{config[CONF_CONVERTER]}', which sets its own scale",
            path=[CONF_SCALE],
        )
    return config


def _off_grid(value: float, scale: float) -> bool:
    steps = value / scale
    return abs(steps - round(steps)) > _GRID_EPS


def _validate_raw_grid(config):
    """min_value and step must land on whole raw steps, or the frontend offers
    values between two raw values that the write rounds away. An error with
    `scale`; with a converter preset only a warning, for existing configs."""
    scale = _effective_scale(config)
    offset = config[CONF_VALUE_OFFSET]
    bad = [
        (key, config[key])
        for key, v in ((CONF_STEP, config[CONF_STEP]), (CONF_MIN_VALUE, config[CONF_MIN_VALUE] - offset))
        if _off_grid(v, scale)
    ]
    if not bad:
        return config
    key, value = bad[0]
    msg = f"{key} ({value:g}) is not a whole number of raw steps (scale {scale:g}, value_offset {offset:g})"
    if CONF_SCALE in config:
        raise cv.Invalid(msg, path=[key])
    _LOGGER.warning("%s: %s", config[CONF_NAME], msg)
    return config


def _validate_encodable_range(config):
    """Reject min/max that cannot be represented on the wire.

    This is the load-bearing config-time check for the write path: it mirrors
    ``decode.h::encode_scaled`` exactly (round the raw step half away from zero
    -- ``llround``, NOT Python's half-to-even ``round()``, which diverges at
    negative half-steps -- then range-check for the byte width and sign), so an
    un-encodable bound is an ``esphome config`` error rather than a runtime
    "value not written" log.
    """
    scale = _effective_scale(config)
    offset = config[CONF_VALUE_OFFSET]
    is_signed = resolve_signed(config)
    width = field_width(config)
    for key in (CONF_MIN_VALUE, CONF_MAX_VALUE):
        raw = llround((config[key] - offset) / scale)
        if CONF_BIT_MASK in config:
            # A bit field holds 0..bit_field_max(mask), unsigned -- the same
            # range VitoNumber::control() enforces before staging the field.
            top = bit_field_max(config[CONF_BIT_MASK])
            if not 0 <= raw <= top:
                raise cv.Invalid(
                    f"{key} ({config[key]}) -> raw {raw} does not fit bit_mask "
                    f"0x{config[CONF_BIT_MASK]:02X} (field values 0..{top}) with converter "
                    f"'{config[CONF_CONVERTER]}' (scale {scale:g}, value_offset {offset:g})",
                    path=[key],
                )
            continue
        if not raw_fits(raw, width, is_signed):
            kind = "signed" if is_signed else "unsigned"
            raise cv.Invalid(
                f"{key} ({config[key]}) -> raw {raw} does not fit {width} "
                f"{kind} byte(s) with converter '{config[CONF_CONVERTER]}' "
                f"(scale {scale:g}, value_offset {offset:g}). Pick a different length/converter or bound.",
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
            # GWG-only; rejected under any other protocol in _final_validate
            # (__init__.py). One mode drives this entity's reads AND writes.
            cv.Optional(CONF_ACCESS): cv.enum(GWG_ACCESS_MODES, lower=True),
            cv.Optional(CONF_STATE_ADDRESS): cv.hex_uint16_t,
            cv.Optional(CONF_LENGTH, default=1): cv.positive_int,
            cv.Optional(CONF_BYTE_OFFSET): cv.int_range(min=0, max=MAX_P300_READ_LENGTH - 1),
            cv.Optional(CONF_BYTE_LENGTH): cv.int_range(min=1, max=4),
            cv.Optional(CONF_CONVERTER, default="noconv"): cv.enum(_WRITABLE_CONVERTERS, lower=True),
            cv.Optional(CONF_VALUE_OFFSET, default=0.0): cv.float_,
            cv.Optional(CONF_SCALE): cv.float_range(min=0.0, min_included=False),
            cv.Optional(CONF_SIGNED): cv.boolean,
            # Writes become a read-modify-write of the one byte (vitohome.h,
            # rmw_pending_); see validate_bit_field for the shape rules.
            cv.Optional(CONF_BIT_MASK): BIT_MASK_SCHEMA,
            cv.Required(CONF_MIN_VALUE): cv.float_,
            cv.Required(CONF_MAX_VALUE): cv.float_,
            cv.Required(CONF_STEP): cv.positive_float,
            cv.Optional(CONF_READ_BACK, default=True): cv.boolean,
            cv.Optional(CONF_UPDATE_INTERVAL): poll_update_interval,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    validate_block_extraction((1, 2, 3, 4)),
    _validate_converter_field_width,
    validate_bit_field(writable=True),
    _validate_scale,
    _validate_encodable_range,
    _validate_raw_grid,
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
    cg.add(var.set_scale(scale_literal(config.get(CONF_SCALE, config[CONF_CONVERTER]))))
    if config[CONF_VALUE_OFFSET]:
        cg.add(var.set_value_offset(cg.RawExpression(repr(float(config[CONF_VALUE_OFFSET])))))
    cg.add(var.set_signed(resolve_signed(config)))
    cg.add(var.set_read_back(config[CONF_READ_BACK]))
    emit_bit_mask(var, config)
    emit_poll_interval(var, poll_ms)

    if CONF_ACCESS in config:
        cg.add(var.set_access(config[CONF_ACCESS]))

    cg.add(parent.register_entity(var))
