import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import (
    CONF_ADDRESS,
    CONF_NAME,
    CONF_OPTIONS,
    CONF_TYPE,
    CONF_UPDATE_INTERVAL,
)

from . import (
    CONF_LENGTH,
    CONF_VITOCONNECT_ID,
    VitoHomeComponent,
    datapoint_expression,
    validate_length_in,
    vitohome_ns,
)

DEPENDENCIES = ["vitohome"]

CONF_CODES = "codes"

# A 9-byte error-history slot: [0]=code, [1..8]=DateTimeBCD. The read length is
# fixed by the on-wire layout, so it is validated rather than configurable.
ERROR_HISTORY_LENGTH = 9

VitoTextSensor = vitohome_ns.class_("VitoTextSensor", text_sensor.TextSensor, cg.Component)
TextSensorType = vitohome_ns.enum("TextSensorType", is_class=True)

TEXT_SENSOR_TYPES = {
    "raw": TextSensorType.RAW_HEX,
    "enum": TextSensorType.ENUM,
    "error_history": TextSensorType.ERROR_HISTORY,
    "device_id": TextSensorType.DEVICE_ID,
    "ascii": TextSensorType.ASCII,
}

# A {raw_value: label} map. Keys are integers (the decoded wire value), values
# are the human-readable labels. Order is irrelevant (lookup is by value).
_VALUE_MAP = cv.Schema({cv.uint32_t: cv.string})

_BASE = {
    cv.GenerateID(CONF_VITOCONNECT_ID): cv.use_id(VitoHomeComponent),
}


def _addressed(extra: dict) -> cv.Schema:
    """A text_sensor schema for a bus-polling type (everything but device_id)."""
    return (
        text_sensor.text_sensor_schema(VitoTextSensor)
        .extend(_BASE)
        .extend(
            {
                cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
                cv.Optional(CONF_UPDATE_INTERVAL): cv.update_interval,
            }
        )
        .extend(extra)
        .extend(cv.COMPONENT_SCHEMA)
    )


CONFIG_SCHEMA = cv.typed_schema(
    {
        "raw": _addressed(
            {
                cv.Optional(CONF_LENGTH, default=1): validate_length_in(1, 4),
            }
        ),
        "enum": _addressed(
            {
                cv.Optional(CONF_LENGTH, default=1): validate_length_in(1, 4),
                cv.Required(CONF_OPTIONS): _VALUE_MAP,
            }
        ),
        "error_history": _addressed(
            {
                # Fixed by the wire layout; accept it explicitly so a typo is a
                # config error, not a silent wrong read.
                cv.Optional(CONF_LENGTH, default=ERROR_HISTORY_LENGTH): cv.int_range(
                    min=ERROR_HISTORY_LENGTH, max=ERROR_HISTORY_LENGTH
                ),
                cv.Optional(CONF_CODES, default={}): _VALUE_MAP,
            }
        ),
        "device_id": (text_sensor.text_sensor_schema(VitoTextSensor).extend(_BASE).extend(cv.COMPONENT_SCHEMA)),
        "ascii": _addressed(
            {
                # Byte-string field width (Sachnummer 7, Herstellnummer 16).
                # No universal default, so require it; capped at one P300 read.
                cv.Required(CONF_LENGTH): cv.int_range(min=1, max=32),
            }
        ),
    },
    key=CONF_TYPE,
    default_type="raw",
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VITOCONNECT_ID])
    # See sensor.py: pop the reserved update_interval before register_component.
    # (device_id has no such key, so this is a no-op there.)
    poll_interval = config.pop(CONF_UPDATE_INTERVAL, None)
    var = await text_sensor.new_text_sensor(config)
    await cg.register_component(var, config)
    cg.add(var.set_type(TEXT_SENSOR_TYPES[config[CONF_TYPE]]))

    if config[CONF_TYPE] == "device_id":
        # No bus reads of its own: it subscribes to the hub's one-shot
        # identification result instead of polling.
        cg.add(parent.register_device_id_sensor(var))
        return

    cg.add(var.set_datapoint(datapoint_expression(config[CONF_NAME], config[CONF_ADDRESS], config[CONF_LENGTH])))

    # add_option(value, label) takes (uint32_t, const char*); ESPHome emits the
    # label as a properly-escaped C++ string literal, so no manual escaping here.
    mapping = config.get(CONF_OPTIONS) or config.get(CONF_CODES) or {}
    for value, label in mapping.items():
        cg.add(var.add_option(value, label))

    if poll_interval is not None:
        cg.add(var.set_poll_interval(int(poll_interval.total_milliseconds)))

    cg.add(parent.register_entity(var))
