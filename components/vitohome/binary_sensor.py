import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import CONF_ADDRESS, CONF_NAME, CONF_UPDATE_INTERVAL

from . import (
    CONF_BYTE_OFFSET,
    CONF_LENGTH,
    CONF_VITOHOME_ID,
    MAX_P300_READ_LENGTH,
    VitoHomeComponent,
    datapoint_expression,
    vitohome_ns,
)

DEPENDENCIES = ["vitohome"]

CONF_BIT_MASK = "bit_mask"

VitoBinarySensor = vitohome_ns.class_("VitoBinarySensor", binary_sensor.BinarySensor, cg.Component)


def _validate_length(config):
    # `length` is the BLOCK read issued at `address` -- always the block base,
    # so P300 gets an aligned telegram -- and `byte_offset` picks the byte the
    # bit lives in. Vitosoft puts status bits deep inside wide blocks
    # (HK_Frostgefahr_aktivA1M1 is bit 135 = byte 16 of the 22-byte block at
    # 0x2500; WPR3_Geraetestatus_Party_HK1 is bit 0 of a 10-byte block, i.e.
    # byte_offset 0 with length 10), which the old 1..4 cap could not express at
    # all -- the generator emitted a "custom handling" comment instead.
    # Capped at the P300 single-telegram limit for the same reason sensor.py
    # caps it there: a wider read works on KW but NAKs on P300.
    length = config[CONF_LENGTH]
    if not 1 <= length <= MAX_P300_READ_LENGTH:
        raise cv.Invalid(
            f"length is a block read and must be 1..{MAX_P300_READ_LENGTH} bytes (got {length})",
            path=[CONF_LENGTH],
        )
    return config


def _validate_offset_within_length(config):
    if config[CONF_BYTE_OFFSET] >= config[CONF_LENGTH]:
        raise cv.Invalid(
            f"byte_offset ({config[CONF_BYTE_OFFSET]}) must be < length ({config[CONF_LENGTH]})",
            path=[CONF_BYTE_OFFSET],
        )
    return config


# Hub-fed link diagnostic: no address, no polling — the hub publishes its own
# Optolink link state into it (online on any successful response; offline on
# start-up verify failure or after 3 consecutive protocol errors). Mirrors the
# device_id text_sensor pattern: a plain framework entity registered with the
# hub, not a VitoBinarySensor.
_CONNECTIVITY_SCHEMA = binary_sensor.binary_sensor_schema(
    binary_sensor.BinarySensor,
    device_class="connectivity",
    entity_category="diagnostic",
).extend(
    {
        cv.GenerateID(CONF_VITOHOME_ID): cv.use_id(VitoHomeComponent),
    }
)

_DATAPOINT_SCHEMA = cv.All(
    binary_sensor.binary_sensor_schema(VitoBinarySensor)
    .extend(
        {
            cv.GenerateID(CONF_VITOHOME_ID): cv.use_id(VitoHomeComponent),
            cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
            cv.Optional(CONF_LENGTH, default=1): cv.positive_int,
            cv.Optional(CONF_BYTE_OFFSET, default=0): cv.int_range(min=0, max=MAX_P300_READ_LENGTH - 1),
            cv.Optional(CONF_BIT_MASK, default=0xFF): cv.hex_uint8_t,
            cv.Optional(CONF_UPDATE_INTERVAL): cv.update_interval,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _validate_length,
    _validate_offset_within_length,
)

CONF_TYPE = "type"
CONFIG_SCHEMA = cv.typed_schema(
    {
        "datapoint": _DATAPOINT_SCHEMA,
        "connectivity": _CONNECTIVITY_SCHEMA,
    },
    default_type="datapoint",
)


async def _connectivity_to_code(config):
    parent = await cg.get_variable(config[CONF_VITOHOME_ID])
    var = await binary_sensor.new_binary_sensor(config)
    cg.add(parent.register_link_sensor(var))


async def to_code(config):
    if config.get(CONF_TYPE) == "connectivity":
        await _connectivity_to_code(config)
        return
    parent = await cg.get_variable(config[CONF_VITOHOME_ID])
    # See sensor.py: pop the reserved update_interval before register_component
    # so it doesn't emit set_update_interval() on our passive entity.
    poll_interval = config.pop(CONF_UPDATE_INTERVAL, None)
    var = await binary_sensor.new_binary_sensor(config)
    await cg.register_component(var, config)

    # Raw-bit read: index the payload directly via byte_offset/bit_mask, so the
    # datapoint converter is irrelevant (always noconv). Length still drives how
    # many bytes are requested.
    cg.add(var.set_datapoint(datapoint_expression(config[CONF_NAME], config[CONF_ADDRESS], config[CONF_LENGTH])))
    cg.add(var.set_byte_offset(config[CONF_BYTE_OFFSET]))
    cg.add(var.set_bit_mask(config[CONF_BIT_MASK]))
    if poll_interval is not None:
        cg.add(var.set_poll_interval(int(poll_interval.total_milliseconds)))

    cg.add(parent.register_entity(var))
