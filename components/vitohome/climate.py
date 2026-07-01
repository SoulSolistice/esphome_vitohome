import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate
from esphome.const import (
    CONF_ADDRESS,
    CONF_MAX_TEMPERATURE,
    CONF_MIN_TEMPERATURE,
    CONF_MODE,
    CONF_NAME,
    CONF_UPDATE_INTERVAL,
    CONF_VISUAL,
)

from . import (
    CONF_READ_BACK,
    CONF_VITOCONNECT_ID,
    VitoHomeComponent,
    datapoint_expression,
    vitohome_ns,
)

DEPENDENCIES = ["vitohome"]

VitoClimate = vitohome_ns.class_("VitoClimate", climate.Climate, cg.Component)

climate_ns = cg.esphome_ns.namespace("climate")
ClimateMode = climate_ns.enum("ClimateMode")

# Coarse climate mode each preset derives. OFF/HEAT cover a weather-compensated
# heating circuit; AUTO is offered for schedule-driven setups that want it.
CLIMATE_MODES = {
    "off": ClimateMode.CLIMATE_MODE_OFF,
    "heat": ClimateMode.CLIMATE_MODE_HEAT,
    "auto": ClimateMode.CLIMATE_MODE_AUTO,
}

CONF_TARGET_ADDRESS = "target_address"
CONF_OPERATING_MODE = "operating_mode"
CONF_STATE_ADDRESS = "state_address"
CONF_PRESETS = "presets"
CONF_WRITE = "write"
CONF_READ = "read"

# One Betriebsart preset. `write` is the command-space byte; `read` is one or
# more state-space bytes that map back to this preset; `name` is a free label;
# `mode` is the coarse climate mode the card shows when this preset is active.
_PRESET_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NAME): cv.string_strict,
        cv.Required(CONF_WRITE): cv.hex_uint8_t,
        cv.Required(CONF_READ): cv.ensure_list(cv.hex_uint8_t),
        cv.Optional(CONF_MODE, default="heat"): cv.enum(CLIMATE_MODES, lower=True),
    }
)


def _validate_presets(value):
    value = cv.ensure_list(_PRESET_SCHEMA)(value)
    if not value:
        raise cv.Invalid("at least one preset is required")
    names = [p[CONF_NAME] for p in value]
    if len(set(names)) != len(names):
        raise cv.Invalid("preset names must be unique")
    return value


# Betriebsart is read at state_address (0x2301-style, actual mode the panel and
# schedule update) and written at address (0x2323-style command register). They
# are different value spaces, so the preset rows bind read<->write per preset.
OPERATING_MODE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
        cv.Optional(CONF_STATE_ADDRESS): cv.hex_uint16_t,
        cv.Required(CONF_PRESETS): _validate_presets,
        cv.Optional(CONF_READ_BACK, default=True): cv.boolean,
    }
)

CONFIG_SCHEMA = (
    climate.climate_schema(VitoClimate)
    .extend(
        {
            cv.GenerateID(CONF_VITOCONNECT_ID): cv.use_id(VitoHomeComponent),
            cv.Required(CONF_TARGET_ADDRESS): cv.hex_uint16_t,
            cv.Optional(CONF_OPERATING_MODE): OPERATING_MODE_SCHEMA,
            cv.Optional(CONF_UPDATE_INTERVAL): cv.update_interval,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


def _read_init(values):
    """Render a list of bytes as a C++ brace initializer for std::vector<uint8_t>."""
    return cg.RawExpression("{" + ", ".join(f"0x{v:02X}" for v in values) + "}")


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VITOCONNECT_ID])
    # See sensor.py: pop the reserved update_interval before register_component.
    poll_interval = config.pop(CONF_UPDATE_INTERVAL, None)
    var = await climate.new_climate(config)
    await cg.register_component(var, config)

    name = config.get(CONF_NAME) or "vito_climate"

    # Setpoint clamp range from the standard visual block (also the HA gauge).
    visual = config.get(CONF_VISUAL, {})
    sp_min = int(visual.get(CONF_MIN_TEMPERATURE, 3))
    sp_max = int(visual.get(CONF_MAX_TEMPERATURE, 37))
    cg.add(var.set_setpoint_range(sp_min, sp_max))

    poll_ms = int(poll_interval.total_milliseconds) if poll_interval is not None else 0

    # Setpoint channel: read == write at target_address, one integer degC byte.
    cg.add(var.configure_setpoint(parent, datapoint_expression(name, config[CONF_TARGET_ADDRESS], 1), poll_ms))

    # Operating-mode channel (optional): read state_address, write address.
    if CONF_OPERATING_MODE in config:
        om = config[CONF_OPERATING_MODE]
        write_addr = om[CONF_ADDRESS]
        read_addr = om.get(CONF_STATE_ADDRESS, write_addr)
        cg.add(var.configure_mode(parent, datapoint_expression(name, read_addr, 1), om[CONF_READ_BACK], poll_ms))
        if read_addr != write_addr:
            cg.add(var.set_mode_write_datapoint(datapoint_expression(name, write_addr, 1)))
        for p in om[CONF_PRESETS]:
            cg.add(var.add_preset(p[CONF_NAME], p[CONF_WRITE], _read_init(p[CONF_READ]), p[CONF_MODE]))
