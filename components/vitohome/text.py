import esphome.codegen as cg
from esphome.components import text
import esphome.config_validation as cv
from esphome.const import CONF_ADDRESS, CONF_NAME, CONF_UPDATE_INTERVAL

from . import CONF_READ_BACK, CONF_VITOCONNECT_ID, VitoHomeComponent, datapoint_expression, vitohome_ns

DEPENDENCIES = ["vitohome"]

VitoText = vitohome_ns.class_("VitoText", text.Text, cg.Component)

# A per-day Schaltzeiten program is always 8 bytes (four ON/OFF switch-point
# pairs); the address is the weekday block (base + day*8). Read and write use
# the same address, so the hub's read-back re-reads exactly what was written.
SCHALTZEITEN_LENGTH = 8

# Canonical string upper bound: "HH:MM-HH:MM" x4 + 3 spaces = 47.
_MAX_LENGTH = 47

CONFIG_SCHEMA = (
    text.text_schema(VitoText, mode="text")
    .extend(
        {
            cv.GenerateID(CONF_VITOCONNECT_ID): cv.use_id(VitoHomeComponent),
            cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
            cv.Optional(CONF_READ_BACK, default=True): cv.boolean,
            cv.Optional(CONF_UPDATE_INTERVAL): cv.update_interval,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VITOCONNECT_ID])
    # See sensor.py: pop the reserved update_interval before register_component.
    poll_interval = config.pop(CONF_UPDATE_INTERVAL, None)
    var = await text.new_text(config, min_length=0, max_length=_MAX_LENGTH)
    await cg.register_component(var, config)

    cg.add(var.set_datapoint(datapoint_expression(config[CONF_NAME], config[CONF_ADDRESS], SCHALTZEITEN_LENGTH)))
    cg.add(var.set_read_back(config[CONF_READ_BACK]))
    if poll_interval is not None:
        cg.add(var.set_poll_interval(int(poll_interval.total_milliseconds)))

    cg.add(parent.register_entity(var))
