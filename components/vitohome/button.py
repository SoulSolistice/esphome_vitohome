import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button

from . import CONF_VITOCONNECT_ID, VitoHomeComponent, vitohome_ns

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
            cv.GenerateID(CONF_VITOCONNECT_ID): cv.use_id(VitoHomeComponent),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VITOCONNECT_ID])
    var = await button.new_button(config)
    await cg.register_component(var, config)
    cg.add(var.set_vitohome_parent(parent))
