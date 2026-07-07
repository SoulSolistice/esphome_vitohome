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
    MAX_P300_READ_LENGTH,
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
    "utf16": TextSensorType.UTF16,
    "scan_result": TextSensorType.SCAN_RESULT,
}

# A {raw_value: label} map. Keys are integers (the decoded wire value), values
# are the human-readable labels. Order is irrelevant (lookup is by value).
_VALUE_MAP = cv.Schema({cv.uint32_t: cv.string})

# Aligned block extraction for `type: enum` (read-only twin of the sensor's
# byte_offset): with byte_offset, `length` is the block read at `address` (the
# block base) and the enum field is the byte_length (default 1, max 4) bytes
# at byte_offset. Read-only, so there is no write side and no state_address.
CONF_BYTE_OFFSET = "byte_offset"
CONF_BYTE_LENGTH = "byte_length"  # enum field width at byte_offset (1..4)


def _validate_enum_extraction(config):
    length = config[CONF_LENGTH]
    if CONF_BYTE_OFFSET in config:
        if not 1 <= length <= MAX_P300_READ_LENGTH:
            raise cv.Invalid(
                f"with byte_offset, length is a block read and must be 1..{MAX_P300_READ_LENGTH} (got {length})",
                path=[CONF_LENGTH],
            )
        field_width = config.get(CONF_BYTE_LENGTH, 1)
        if config[CONF_BYTE_OFFSET] + field_width > length:
            raise cv.Invalid(
                f"byte_offset ({config[CONF_BYTE_OFFSET]}) + byte_length ({field_width}) must be <= length ({length})",
                path=[CONF_BYTE_OFFSET],
            )
    else:
        if CONF_BYTE_LENGTH in config:
            raise cv.Invalid("byte_length requires byte_offset", path=[CONF_BYTE_LENGTH])
        if not 1 <= length <= 4:
            raise cv.Invalid(f"length must be between 1 and 4 bytes (got {length})", path=[CONF_LENGTH])
    return config


def _validate_code_bytes(config):
    """The error_history codes map is keyed by the decoded wire code BYTE, so
    every key must fit 0..0xFF -- a wider key is a dead entry that can never
    match the 8-bit code. Mirrors event.py::_validate_codes so both fault
    surfaces reject the same out-of-range keys, with a clear message. Applied as
    a post-validator (a voluptuous key-marker that RAISES is swallowed into a
    generic 'extra keys' error, so the range check must run after key parsing)."""
    for code in config.get(CONF_CODES, {}):
        if not 0 <= code <= 0xFF:
            raise cv.Invalid(
                f"fault code 0x{code:X} does not fit one byte (0..0xFF)",
                path=[CONF_CODES],
            )
    return config


_BASE = {
    cv.GenerateID(CONF_VITOCONNECT_ID): cv.use_id(VitoHomeComponent),
}


def _validate_even_length(value):
    """UTF-16 code units are 2 bytes; decode.h::decode_utf16 rejects odd widths
    at runtime, so reject them at config time instead."""
    if value % 2 != 0:
        raise cv.Invalid(f"utf16 length must be even (UTF-16LE code units are 2 bytes; got {value})")
    return value


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
        "enum": cv.All(
            _addressed(
                {
                    cv.Optional(CONF_LENGTH, default=1): cv.positive_int,
                    cv.Optional(CONF_BYTE_OFFSET): cv.int_range(min=0, max=MAX_P300_READ_LENGTH - 1),
                    cv.Optional(CONF_BYTE_LENGTH): cv.int_range(min=1, max=4),
                    cv.Required(CONF_OPTIONS): _VALUE_MAP,
                }
            ),
            _validate_enum_extraction,
        ),
        "error_history": cv.All(
            _addressed(
                {
                    # Fixed by the wire layout; accept it explicitly so a typo is a
                    # config error, not a silent wrong read.
                    cv.Optional(CONF_LENGTH, default=ERROR_HISTORY_LENGTH): cv.int_range(
                        min=ERROR_HISTORY_LENGTH, max=ERROR_HISTORY_LENGTH
                    ),
                    cv.Optional(CONF_CODES, default={}): _VALUE_MAP,
                }
            ),
            _validate_code_bytes,
        ),
        "device_id": (text_sensor.text_sensor_schema(VitoTextSensor).extend(_BASE).extend(cv.COMPONENT_SCHEMA)),
        # No address: the hub feeds it the raw scan-console result line
        # (queue_raw_read / queue_raw_write), exactly like device_id is fed by
        # identification.
        "scan_result": (text_sensor.text_sensor_schema(VitoTextSensor).extend(_BASE).extend(cv.COMPONENT_SCHEMA)),
        "ascii": _addressed(
            {
                # Byte-string field width (Sachnummer 7, Herstellnummer 16).
                # No universal default, so require it; capped at one P300 read.
                cv.Required(CONF_LENGTH): cv.int_range(min=1, max=32),
            }
        ),
        "utf16": _addressed(
            {
                # UTF-16LE label width in BYTES (Beschriftung_HK* = 40 = 20 chars).
                # Must be even (code units are 2 bytes) -- enforced here so an odd
                # width is an `esphome config` error, not a per-poll runtime
                # decode failure; capped at one P300 read.
                cv.Required(CONF_LENGTH): cv.All(cv.int_range(min=2, max=40), _validate_even_length),
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

    if config[CONF_TYPE] == "scan_result":
        # No bus reads of its own: the hub publishes the raw scan-console result
        # line to it (queue_raw_read / queue_raw_write).
        cg.add(parent.register_raw_result_sensor(var))
        return

    cg.add(var.set_datapoint(datapoint_expression(config[CONF_NAME], config[CONF_ADDRESS], config[CONF_LENGTH])))
    if CONF_BYTE_OFFSET in config:
        # enum block extraction: read `length` bytes at the block base, decode
        # the byte_length-wide field at byte_offset.
        cg.add(var.set_extract_byte(config[CONF_BYTE_OFFSET]))
        if CONF_BYTE_LENGTH in config:
            cg.add(var.set_extract_len(config[CONF_BYTE_LENGTH]))

    # add_option(value, label) takes (uint32_t, const char*); ESPHome emits the
    # label as a properly-escaped C++ string literal, so no manual escaping here.
    mapping = config.get(CONF_OPTIONS) or config.get(CONF_CODES) or {}
    for value, label in mapping.items():
        cg.add(var.add_option(value, label))

    if poll_interval is not None:
        cg.add(var.set_poll_interval(int(poll_interval.total_milliseconds)))

    cg.add(parent.register_entity(var))
