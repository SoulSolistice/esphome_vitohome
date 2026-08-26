import esphome.codegen as cg
from esphome.components import text
import esphome.config_validation as cv
from esphome.const import CONF_ADDRESS, CONF_NAME, CONF_UPDATE_INTERVAL

from . import (
    CONF_ACCESS,
    CONF_READ_BACK,
    CONF_VITOHOME_ID,
    GWG_ACCESS_MODES,
    VitoHomeComponent,
    datapoint_expression,
    emit_poll_interval,
    pop_poll_interval,
    vitohome_ns,
)

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
            cv.GenerateID(CONF_VITOHOME_ID): cv.use_id(VitoHomeComponent),
            cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
            # GWG-only; rejected under any other protocol in _final_validate
            # (__init__.py). One mode drives this entity's reads AND writes.
            cv.Optional(CONF_ACCESS): cv.enum(GWG_ACCESS_MODES, lower=True),
            cv.Optional(CONF_READ_BACK, default=True): cv.boolean,
            cv.Optional(CONF_UPDATE_INTERVAL): cv.update_interval,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VITOHOME_ID])
    poll_ms = pop_poll_interval(config)
    var = await text.new_text(config, min_length=0, max_length=_MAX_LENGTH)
    await cg.register_component(var, config)

    cg.add(var.set_datapoint(datapoint_expression(config[CONF_NAME], config[CONF_ADDRESS], SCHALTZEITEN_LENGTH)))
    cg.add(var.set_read_back(config[CONF_READ_BACK]))
    emit_poll_interval(var, poll_ms)

    if CONF_ACCESS in config:
        cg.add(var.set_access(config[CONF_ACCESS]))

    cg.add(parent.register_entity(var))
