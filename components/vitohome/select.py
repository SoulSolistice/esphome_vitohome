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
    raw_fits,
    vitohome_ns,
)

DEPENDENCIES = ["vitohome"]


# Optional read/state address, distinct from the command (write) CONF_ADDRESS,
# for mode controls whose live value is read elsewhere (read/write split).
# Aligned block extraction on the STATE read (mirrors sensor.py): with
# byte_offset, `length` is the block read at the state address and the enum
# field is the byte_length (default 1) bytes at byte_offset. The write still
# targets CONF_ADDRESS -- the field's own register -- which is why byte_offset
# requires an explicit state_address (writing field-width bytes at the block
# base would hit the wrong register).
def _field_width(config):
    """The enum value width: byte_length with extraction, else length."""
    if CONF_BYTE_OFFSET in config:
        return config.get(CONF_BYTE_LENGTH, 1)
    return config[CONF_LENGTH]


def _validate_length_and_extraction(config):
    length = config[CONF_LENGTH]
    if CONF_BYTE_OFFSET in config:
        # length is the block read at the state address (P300 single-telegram
        # cap); the field is byte_length bytes at byte_offset inside it.
        if not 1 <= length <= MAX_P300_READ_LENGTH:
            raise cv.Invalid(
                f"with byte_offset, length is a block read and must be 1..{MAX_P300_READ_LENGTH} (got {length})",
                path=[CONF_LENGTH],
            )
        if CONF_STATE_ADDRESS not in config:
            raise cv.Invalid(
                "byte_offset requires state_address: the aligned block is read at "
                "state_address while address stays the field's own write register",
                path=[CONF_BYTE_OFFSET],
            )
        field_width = config.get(CONF_BYTE_LENGTH, 1)
        if config[CONF_BYTE_OFFSET] + field_width > length:
            raise cv.Invalid(
                f"byte_offset ({config[CONF_BYTE_OFFSET]}) + byte_length ({field_width}) must be <= length ({length})",
                path=[CONF_BYTE_OFFSET],
            )
    else:
        if CONF_BYTE_LENGTH in config:
            raise cv.Invalid("byte_length requires byte_offset", path=[CONF_BYTE_LENGTH])
        if length not in (1, 2):
            raise cv.Invalid(f"length must be 1 or 2 bytes (got {length})", path=[CONF_LENGTH])
    return config


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
    width = _field_width(config)
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
    _validate_length_and_extraction,
    _validate_options,
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VITOHOME_ID])
    # See sensor.py: pop the reserved update_interval before register_component.
    poll_interval = config.pop(CONF_UPDATE_INTERVAL, None)
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
    if CONF_BYTE_OFFSET in config:
        # Block extraction: the state read is `length` bytes at the state
        # address; the enum field is byte_length bytes at byte_offset. The
        # write datapoint carries the FIELD width so control() writes exactly
        # the field's bytes to the field's own register.
        field_width = config.get(CONF_BYTE_LENGTH, 1)
        cg.add(var.set_write_datapoint(datapoint_expression(config[CONF_NAME], write_addr, field_width)))
        cg.add(var.set_extract_byte(config[CONF_BYTE_OFFSET]))
        if CONF_BYTE_LENGTH in config:
            cg.add(var.set_extract_len(config[CONF_BYTE_LENGTH]))
    elif read_addr != write_addr:
        cg.add(var.set_write_datapoint(datapoint_expression(config[CONF_NAME], write_addr, config[CONF_LENGTH])))
    cg.add(var.set_read_back(config[CONF_READ_BACK]))
    if poll_interval is not None:
        cg.add(var.set_poll_interval(int(poll_interval.total_milliseconds)))

    cg.add(parent.register_entity(var))
