import esphome.codegen as cg
from esphome.components import event
import esphome.config_validation as cv
from esphome.const import CONF_ADDRESS, CONF_NAME, CONF_UPDATE_INTERVAL

from . import CONF_LENGTH, CONF_VITOCONNECT_ID, VitoHomeComponent, datapoint_expression, validate_length_in, vitohome_ns

DEPENDENCIES = ["vitohome"]

CONF_CODES = "codes"

VitoEvent = vitohome_ns.class_("VitoEvent", event.Event, cg.Component)

# {code_byte: label}, same shape as text_sensor type: error_history. The keys
# define the event-type space HA sees: each code fires as its hex string
# ("0x10"), plus the two built-ins "cleared" (slot went to 0x00) and "unknown"
# (a code outside this map -- raw value in the log).
_CODES_MAP = cv.Schema({cv.uint32_t: cv.string})


def _validate_codes(config):
    for code in config[CONF_CODES]:
        if not 0 <= code <= 0xFF:
            raise cv.Invalid(
                f"fault code 0x{code:X} does not fit one byte",
                path=[CONF_CODES],
            )
    if not config[CONF_CODES]:
        raise cv.Invalid("at least one fault code is required", path=[CONF_CODES])
    return config


CONFIG_SCHEMA = cv.All(
    event.event_schema(VitoEvent)
    .extend(
        {
            cv.GenerateID(CONF_VITOCONNECT_ID): cv.use_id(VitoHomeComponent),
            # The newest-fault slot (FA01), e.g. 0x7507 on the B3HA.
            cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
            # Slot layout is code byte + 8-byte BCD timestamp; only the code
            # byte drives events, but reading the full slot (default 9) keeps
            # the wire read identical to the error_history text_sensor's.
            cv.Optional(CONF_LENGTH, default=9): validate_length_in(1, 9),
            cv.Required(CONF_CODES): _CODES_MAP,
            cv.Optional(CONF_UPDATE_INTERVAL): cv.update_interval,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _validate_codes,
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VITOCONNECT_ID])
    # See sensor.py: pop the reserved update_interval before register_component.
    poll_interval = config.pop(CONF_UPDATE_INTERVAL, None)

    event_types = [f"0x{code:02X}" for code in config[CONF_CODES]]
    event_types += ["cleared", "unknown"]
    var = await event.new_event(config, event_types=event_types)
    await cg.register_component(var, config)

    for code, label in config[CONF_CODES].items():
        cg.add(var.add_code(code, label))

    cg.add(var.set_datapoint(datapoint_expression(config[CONF_NAME], config[CONF_ADDRESS], config[CONF_LENGTH])))
    if poll_interval is not None:
        cg.add(var.set_poll_interval(int(poll_interval.total_milliseconds)))

    cg.add(parent.register_entity(var))
