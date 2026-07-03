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
    VitoHomeComponent,
    datapoint_expression,
    raw_fits,
    validate_length_in,
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

VitoSwitch = vitohome_ns.class_("VitoSwitch", switch.Switch, cg.Component)


def _validate_switch(config):
    length = config[CONF_LENGTH]
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
        if not raw_fits(value, length, is_signed=False):
            raise cv.Invalid(
                f"value {value} does not fit {length} unsigned byte(s)",
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
            cv.Optional(CONF_LENGTH, default=1): validate_length_in(1, 2),
            cv.Optional(CONF_ON_VALUE, default=1): cv.uint32_t,
            cv.Optional(CONF_OFF_VALUE, default=0): cv.uint32_t,
            cv.Optional(CONF_ON_VALUES): cv.ensure_list(cv.uint32_t),
            cv.Optional(CONF_READ_BACK, default=True): cv.boolean,
            cv.Optional(CONF_UPDATE_INTERVAL): cv.update_interval,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
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
    if read_addr != write_addr:
        cg.add(var.set_write_datapoint(datapoint_expression(config[CONF_NAME], write_addr, config[CONF_LENGTH])))
    cg.add(var.set_read_back(config[CONF_READ_BACK]))
    if poll_interval is not None:
        cg.add(var.set_poll_interval(int(poll_interval.total_milliseconds)))

    cg.add(parent.register_entity(var))
