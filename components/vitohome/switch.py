import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import (
    CONF_ADDRESS,
    CONF_NAME,
    CONF_RESTORE_MODE,
    CONF_UPDATE_INTERVAL,
)

from . import (
    CONF_LENGTH,
    CONF_READ_BACK,
    CONF_VITOCONNECT_ID,
    MAX_P300_READ_LENGTH,
    VitoHomeComponent,
    datapoint_expression,
    raw_fits,
    vitohome_ns,
)

DEPENDENCIES = ["vitohome"]

# Optional read/state address, distinct from the command (write) CONF_ADDRESS
# -- the same read/write split select.py supports (e.g. NRx Partybetrieb
# command 0x2330 vs BedienPartybetrieb state 0x2303).
CONF_STATE_ADDRESS = "state_address"
CONF_ON_VALUE = "on_value"
CONF_OFF_VALUE = "off_value"
CONF_ON_VALUES = "on_values"
# Aligned block extraction on the STATE read -- identical semantics to
# select.py: with byte_offset, `length` is the block read at the state address
# and the boolean field is the byte_length (default 1) bytes at byte_offset.
# The write still targets CONF_ADDRESS (the field's own register), which is
# why byte_offset requires an explicit state_address.
CONF_BYTE_OFFSET = "byte_offset"
CONF_BYTE_LENGTH = "byte_length"  # field width at byte_offset (1..2)


def _field_width(config):
    """The on/off value width: byte_length with extraction, else length."""
    if CONF_BYTE_OFFSET in config:
        return config.get(CONF_BYTE_LENGTH, 1)
    return config[CONF_LENGTH]


def _validate_length_and_extraction(config):
    length = config[CONF_LENGTH]
    if CONF_BYTE_OFFSET in config:
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


VitoSwitch = vitohome_ns.class_("VitoSwitch", switch.Switch, cg.Component)


def _validate_switch(config):
    width = _field_width(config)
    on_value = config[CONF_ON_VALUE]
    off_value = config[CONF_OFF_VALUE]
    if on_value == off_value:
        raise cv.Invalid("on_value and off_value must differ", path=[CONF_ON_VALUE])
    # on_values is what READS as on; on_value is what is WRITTEN for on. The
    # default read set is [on_value]; an explicit list replaces it (for
    # registers that report extra "on-ish" states, e.g. active-by-schedule).
    on_values = config.get(CONF_ON_VALUES) or [on_value]
    if off_value in on_values:
        raise cv.Invalid(
            f"off_value {off_value} cannot also be in on_values",
            path=[CONF_OFF_VALUE],
        )
    for value in {on_value, off_value, *on_values}:
        if not raw_fits(value, width, is_signed=False):
            raise cv.Invalid(
                f"value {value} does not fit {width} unsigned byte(s)",
                path=[CONF_ON_VALUE],
            )
    # State always comes from the device (poll + read-back). Any other restore
    # mode would replay a remembered state at boot, i.e. WRITE to the heater
    # on every reboot -- rejected rather than silently ignored.
    if str(config[CONF_RESTORE_MODE]) != "DISABLED":
        raise cv.Invalid(
            "vitohome switches take their state from the device; boot-time "
            "restore would write to the heater. restore_mode must stay DISABLED",
            path=[CONF_RESTORE_MODE],
        )
    return config


CONFIG_SCHEMA = cv.All(
    switch.switch_schema(
        VitoSwitch,
        block_inverted=True,
        default_restore_mode="DISABLED",
    )
    .extend(
        {
            cv.GenerateID(CONF_VITOCONNECT_ID): cv.use_id(VitoHomeComponent),
            cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
            cv.Optional(CONF_STATE_ADDRESS): cv.hex_uint16_t,
            cv.Optional(CONF_LENGTH, default=1): cv.positive_int,
            cv.Optional(CONF_BYTE_OFFSET): cv.int_range(min=0, max=MAX_P300_READ_LENGTH - 1),
            cv.Optional(CONF_BYTE_LENGTH): cv.int_range(min=1, max=2),
            cv.Optional(CONF_ON_VALUE, default=1): cv.uint32_t,
            cv.Optional(CONF_OFF_VALUE, default=0): cv.uint32_t,
            cv.Optional(CONF_ON_VALUES): cv.ensure_list(cv.uint32_t),
            cv.Optional(CONF_READ_BACK, default=True): cv.boolean,
            cv.Optional(CONF_UPDATE_INTERVAL): cv.update_interval,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _validate_length_and_extraction,
    _validate_switch,
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VITOCONNECT_ID])
    # See sensor.py: pop the reserved update_interval before register_component.
    poll_interval = config.pop(CONF_UPDATE_INTERVAL, None)

    var = await switch.new_switch(config)
    await cg.register_component(var, config)

    cg.add(var.set_on_value(config[CONF_ON_VALUE]))
    cg.add(var.set_off_value(config[CONF_OFF_VALUE]))
    for value in config.get(CONF_ON_VALUES) or [config[CONF_ON_VALUE]]:
        cg.add(var.add_on_state_value(value))

    # CONF_ADDRESS is the command (write) address; with CONF_STATE_ADDRESS the
    # live value is read there instead -- identical to select.py.
    write_addr = config[CONF_ADDRESS]
    read_addr = config.get(CONF_STATE_ADDRESS, write_addr)
    cg.add(var.set_datapoint(datapoint_expression(config[CONF_NAME], read_addr, config[CONF_LENGTH])))
    if CONF_BYTE_OFFSET in config:
        # Block extraction: see select.py -- the write datapoint carries the
        # FIELD width so write_state() writes exactly the field's bytes to the
        # field's own register.
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
