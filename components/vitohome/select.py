import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv
from esphome.const import CONF_ADDRESS, CONF_NAME, CONF_OPTIONS, CONF_UPDATE_INTERVAL

from . import (
    CONF_BYTE_LENGTH,
    CONF_BYTE_OFFSET,
    CONF_LENGTH,
    CONF_READ_BACK,
    CONF_STATE_ADDRESS,
    CONF_VITOHOME_ID,
    MAX_P300_READ_LENGTH,
    VitoHomeComponent,
    datapoint_expression,
    emit_poll_interval,
    emit_write_target,
    field_width,
    pop_poll_interval,
    raw_fits,
    validate_block_extraction,
    vitohome_ns,
)

DEPENDENCIES = ["vitohome"]

# Optional read/state address, distinct from the command (write) CONF_ADDRESS,
# for mode controls whose live value is read elsewhere (read/write split), plus
# aligned block extraction on the STATE read. The rules are identical across
# number/select/switch and live in validate_block_extraction() / field_width() /
# emit_write_target() in __init__.py.
VitoSelect = vitohome_ns.class_("VitoSelect", select.Select, cg.Component)

# {raw_value: label}. Insertion order defines the option index order, so the
# YAML author controls how options appear in Home Assistant.
_OPTIONS_MAP = cv.Schema({cv.int_: cv.string})


def _validate_options(config):
    options = config[CONF_OPTIONS]
    if not options:
        raise cv.Invalid("at least one option is required", path=[CONF_OPTIONS])
    # Enum writes are unsigned raw values; each must fit the FIELD width (the
    # extracted byte_length with byte_offset, else length).
    width = field_width(config)
    for value in options:
        if not raw_fits(value, width, is_signed=False):
            raise cv.Invalid(
                f"option value {value} does not fit {width} unsigned byte(s)",
                path=[CONF_OPTIONS],
            )
    labels = list(options.values())
    if len(set(labels)) != len(labels):
        raise cv.Invalid("option labels must be unique", path=[CONF_OPTIONS])
    return config


CONFIG_SCHEMA = cv.All(
    select.select_schema(VitoSelect)
    .extend(
        {
            cv.GenerateID(CONF_VITOHOME_ID): cv.use_id(VitoHomeComponent),
            cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
            cv.Optional(CONF_STATE_ADDRESS): cv.hex_uint16_t,
            cv.Optional(CONF_LENGTH, default=1): cv.positive_int,
            cv.Optional(CONF_BYTE_OFFSET): cv.int_range(min=0, max=MAX_P300_READ_LENGTH - 1),
            cv.Optional(CONF_BYTE_LENGTH): cv.int_range(min=1, max=2),
            cv.Required(CONF_OPTIONS): _OPTIONS_MAP,
            cv.Optional(CONF_READ_BACK, default=True): cv.boolean,
            cv.Optional(CONF_UPDATE_INTERVAL): cv.update_interval,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    validate_block_extraction((1, 2)),
    _validate_options,
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VITOHOME_ID])
    poll_ms = pop_poll_interval(config)
    options = config[CONF_OPTIONS]
    labels = list(options.values())

    var = await select.new_select(config, options=labels)
    await cg.register_component(var, config)

    # raw_values_ is the parallel list of wire values in option-index order.
    for value in options:
        cg.add(var.add_raw_value(value))

    # CONF_ADDRESS is the command (write) address. When CONF_STATE_ADDRESS is
    # given, the live value is read there instead (a read/write address split,
    # e.g. NRx Partybetrieb command 0x2330 vs BedienPartybetrieb state 0x2303):
    # poll and read-back use the state datapoint, control() writes the command.
    write_addr = config[CONF_ADDRESS]
    read_addr = config.get(CONF_STATE_ADDRESS, write_addr)
    cg.add(var.set_datapoint(datapoint_expression(config[CONF_NAME], read_addr, config[CONF_LENGTH])))
    emit_write_target(var, config, read_addr, write_addr)
    cg.add(var.set_read_back(config[CONF_READ_BACK]))
    emit_poll_interval(var, poll_ms)

    cg.add(parent.register_entity(var))
