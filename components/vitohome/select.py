import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from esphome.const import (
    CONF_ADDRESS,
    CONF_NAME,
    CONF_OPTIONS,
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

VitoSelect = vitohome_ns.class_("VitoSelect", select.Select, cg.Component)

# {raw_value: label}. Insertion order defines the option index order, so the
# YAML author controls how options appear in Home Assistant.
_OPTIONS_MAP = cv.Schema({cv.int_: cv.string})


def _validate_options(config):
    options = config[CONF_OPTIONS]
    if not options:
        raise cv.Invalid("at least one option is required", path=[CONF_OPTIONS])
    # Enum writes are unsigned raw values; each must fit the configured width.
    for value in options:
        if not raw_fits(value, config[CONF_LENGTH], is_signed=False):
            raise cv.Invalid(
                f"option value {value} does not fit {config[CONF_LENGTH]} "
                f"unsigned byte(s)",
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
            cv.GenerateID(CONF_VITOCONNECT_ID): cv.use_id(VitoHomeComponent),
            cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
            cv.Optional(CONF_LENGTH, default=1): validate_length_in(1, 2),
            cv.Required(CONF_OPTIONS): _OPTIONS_MAP,
            cv.Optional(CONF_READ_BACK, default=True): cv.boolean,
            cv.Optional(CONF_UPDATE_INTERVAL): cv.update_interval,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _validate_options,
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VITOCONNECT_ID])
    options = config[CONF_OPTIONS]
    labels = list(options.values())

    var = await select.new_select(config, options=labels)
    await cg.register_component(var, config)

    # raw_values_ is the parallel list of wire values in option-index order.
    for value in options:
        cg.add(var.add_raw_value(value))

    cg.add(
        var.set_datapoint(
            datapoint_expression(
                config[CONF_NAME], config[CONF_ADDRESS], config[CONF_LENGTH]
            )
        )
    )
    cg.add(var.set_read_back(config[CONF_READ_BACK]))
    if CONF_UPDATE_INTERVAL in config:
        cg.add(var.set_poll_interval(int(config[CONF_UPDATE_INTERVAL].total_milliseconds)))

    cg.add(parent.register_entity(var))
