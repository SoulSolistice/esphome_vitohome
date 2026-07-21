import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv

from . import CONF_VITOHOME_ID, VitoHomeComponent, vitohome_ns

DEPENDENCIES = ["vitohome"]

VitoRefreshButton = vitohome_ns.class_("VitoRefreshButton", button.Button, cg.Component)

# One button type for now: force-refresh. Pressing it (from the HA UI or via
# button.press in an automation) calls the hub's refresh_all(), which marks
# every registered datapoint due on the next scheduler tick -- the existing
# queue discipline throttles the burst, and the hub debounces repeats within
# 5 s. ESPHome-side automations can call id(vito).refresh_all() directly.
CONFIG_SCHEMA = (
    button.button_schema(
        VitoRefreshButton,
        icon="mdi:refresh",
        entity_category="diagnostic",
    )
    .extend(
        {
            cv.GenerateID(CONF_VITOHOME_ID): cv.use_id(VitoHomeComponent),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VITOHOME_ID])
    var = await button.new_button(config)
    await cg.register_component(var, config)
    cg.add(var.set_vitohome_parent(parent))
