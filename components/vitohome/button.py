import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv
from esphome.const import CONF_ADDRESS, CONF_NAME, CONF_TYPE

from . import (
    ACCESS_RPC,
    CONF_ACCESS,
    CONF_PREFIX,
    CONF_VITOHOME_ID,
    RPC_PREFIX_SCHEMA,
    VitoHomeComponent,
    datapoint_expression,
    vitohome_ns,
)

DEPENDENCIES = ["vitohome"]

VitoRefreshButton = vitohome_ns.class_("VitoRefreshButton", button.Button, cg.Component)
VitoActionButton = vitohome_ns.class_("VitoActionButton", button.Button, cg.Component)

# type: refresh (the default) -- force-refresh. Pressing it (from the HA UI or
# via button.press in an automation) calls the hub's refresh_all(), which marks
# every registered datapoint due on the next scheduler tick -- the existing
# queue discipline throttles the burst, and the hub debounces repeats within
# 5 s. ESPHome-side automations can call id(vito).refresh_all() directly.
_REFRESH_SCHEMA = (
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

# type: action -- one remote procedure call per press (P300 only; see
# ACCESS_RPC in __init__.py): `access: rpc` is required and is the only value,
# `prefix:` is the call's parameter where the procedure takes one. A press RUNS
# the procedure on the controller (clear the fault history, reset a circuit's
# parameters, step a boiler sequence), so the catalogs emit every action
# disabled_by_default with a warning. _final_validate rejects it under KW/GWG.
_ACTION_SCHEMA = (
    button.button_schema(
        VitoActionButton,
        icon="mdi:play-circle-outline",
        entity_category="config",
    )
    .extend(
        {
            cv.GenerateID(CONF_VITOHOME_ID): cv.use_id(VitoHomeComponent),
            cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
            cv.Required(CONF_ACCESS): cv.one_of(ACCESS_RPC, lower=True),
            cv.Optional(CONF_PREFIX): RPC_PREFIX_SCHEMA,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)

CONFIG_SCHEMA = cv.typed_schema(
    {
        "refresh": _REFRESH_SCHEMA,
        "action": _ACTION_SCHEMA,
    },
    key=CONF_TYPE,
    default_type="refresh",
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VITOHOME_ID])
    var = await button.new_button(config)
    await cg.register_component(var, config)
    cg.add(var.set_vitohome_parent(parent))
    if config[CONF_TYPE] != "action":
        return
    # Length 1 is a placeholder: a procedure call sends its prefix, not a length.
    cg.add(var.set_datapoint(datapoint_expression(config[CONF_NAME], config[CONF_ADDRESS], 1)))
    cg.add(var.set_rpc_prefix(config.get(CONF_PREFIX, [])))
    cg.add(parent.register_entity(var))
