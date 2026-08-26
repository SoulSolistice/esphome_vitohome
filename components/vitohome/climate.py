import logging

import esphome.codegen as cg
from esphome.components import climate
import esphome.config_validation as cv
from esphome.const import (
    CONF_ADDRESS,
    CONF_ID,
    CONF_MAX_TEMPERATURE,
    CONF_MIN_TEMPERATURE,
    CONF_MODE,
    CONF_NAME,
    CONF_UPDATE_INTERVAL,
    CONF_VISUAL,
)
from esphome.core import ID

from . import (
    CONF_ACCESS,
    CONF_READ_BACK,
    CONF_STATE_ADDRESS,
    CONF_VITOHOME_ID,
    GWG_ACCESS_MODES,
    VitoClimatePreset,
    VitoHomeComponent,
    cpp_string_literal,
    datapoint_expression,
    pop_poll_interval,
    vitohome_ns,
)

DEPENDENCIES = ["vitohome"]

_LOGGER = logging.getLogger(__name__)

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
        # REQUIRED, deliberately (2026-08-26). This was `Optional(default="heat")`,
        # which silently classified every unannotated preset -- including a
        # Standby/Frostschutz row -- as CLIMATE_MODE_HEAT. Two things then follow
        # from control()'s documented "first preset with that mode wins" rule: the
        # card shows "heating" while the burner is off, and an
        # `hvac_mode: heat` tap from Home Assistant, a voice assistant or an
        # automation resolves to whichever such preset is listed first -- so a
        # config that happens to list Standby first turns the heating OFF on a
        # request to turn it on. There is no correct value to guess here (only the
        # installer knows what each Betriebsart byte does on their unit), so the
        # schema asks instead of assuming. Breaking for configs that omitted
        # `mode:`; the error message says exactly what to add.
        cv.Required(CONF_MODE): cv.enum(CLIMATE_MODES, lower=True),
    }
)


def _validate_presets(value):
    value = cv.ensure_list(_PRESET_SCHEMA)(value)
    if not value:
        raise cv.Invalid("at least one preset is required")
    names = [p[CONF_NAME] for p in value]
    if len(set(names)) != len(names):
        raise cv.Invalid("preset names must be unique")
    # A state byte must resolve to exactly one preset: on_mode_read takes the
    # FIRST preset whose read set contains the byte, so a duplicate across two
    # presets would silently shadow the later one and misreport the mode.
    seen_reads: dict = {}
    for p in value:
        for rv in p[CONF_READ]:
            if rv in seen_reads:
                raise cv.Invalid(
                    f"read value 0x{rv:02X} appears in presets "
                    f"'{seen_reads[rv]}' and '{p[CONF_NAME]}'; each state byte "
                    f"must map to exactly one preset"
                )
            seen_reads[rv] = p[CONF_NAME]
    # Several presets sharing one HVAC mode is legitimate and common (Normal,
    # Reduziert and Heizen+Warmwasser are all "heat"), so this is a warning and
    # not an error. But VitoClimate::control() resolves a bare mode tap through
    # first_preset_with_mode_(), i.e. FIRST MATCH IN LIST ORDER -- which is easy
    # to be surprised by from the Home Assistant side, where the thermostat card
    # and voice assistants send exactly such bare mode calls. Name the winner
    # explicitly so list order is a decision rather than an accident.
    by_mode: dict = {}
    for p in value:
        by_mode.setdefault(str(p[CONF_MODE]), []).append(p[CONF_NAME])
    for mode, names in by_mode.items():
        if len(names) > 1:
            _LOGGER.warning(
                "vitohome climate: presets %s all map to hvac mode '%s'; a bare "
                "'%s' mode call (HA thermostat card, voice assistant, automation) "
                "writes the FIRST of them, '%s'. Reorder the presets if that is not "
                "the one you want as the default for '%s'.",
                ", ".join(f"'{n}'" for n in names),
                mode,
                mode,
                names[0],
                mode,
            )
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
        # GWG-only (rejected under any other protocol in _final_validate,
        # __init__.py), and separate from the setpoint channel's `access:` --
        # the two channels are different registers and nothing says they share
        # an access space. Betriebsart both reads and writes, so a read-only
        # mode (the two KMBUS ones) is rejected there too.
        cv.Optional(CONF_ACCESS): cv.enum(GWG_ACCESS_MODES, lower=True),
    }
)


def _validate_setpoint_range(config):
    """The setpoint is encoded as a single unsigned whole-degree degC byte
    (vito_climate.cpp: static_cast<uint8_t>(lroundf(t)) after clamping the target
    to [setpoint_min_, setpoint_max_]). The clamp bounds therefore have to be
    whole numbers in 0..255, or they corrupt the setpoint on the wire:

      * a negative bound wraps to a large positive value,
      * a bound above 255 wraps low (e.g. 300 -> 300 & 0xFF == 44), and
      * a fractional bound is silently truncated to a whole degree by the
        ``int()`` in to_code() below, quietly moving the clamp.

    Reject all three at config time rather than letting them reach the wire.
    to_code()'s ``int()`` is lossless once this validator has run."""
    visual = config.get(CONF_VISUAL, {})
    bounds = {}
    for key in (CONF_MIN_TEMPERATURE, CONF_MAX_TEMPERATURE):
        if key not in visual:
            continue
        value = float(visual[key])
        if value != int(value):
            raise cv.Invalid(
                f"visual.{key} must be a whole number of degrees: the vitohome "
                "climate setpoint is written as an integer degC byte, so a "
                "fractional bound would be silently truncated on the wire",
                [CONF_VISUAL, key],
            )
        if value < 0:
            raise cv.Invalid(
                f"visual.{key} must be >= 0: the vitohome climate setpoint is "
                "written as an unsigned degC byte, so a negative bound would wrap "
                "to a large positive temperature on the wire",
                [CONF_VISUAL, key],
            )
        if value > 255:
            raise cv.Invalid(
                f"visual.{key} must be <= 255: the vitohome climate setpoint is "
                "written as an unsigned degC byte, so a bound above 255 would wrap "
                "to a low temperature on the wire",
                [CONF_VISUAL, key],
            )
        bounds[key] = value
    if (
        CONF_MIN_TEMPERATURE in bounds
        and CONF_MAX_TEMPERATURE in bounds
        and bounds[CONF_MIN_TEMPERATURE] > bounds[CONF_MAX_TEMPERATURE]
    ):
        raise cv.Invalid(
            "visual.max_temperature must be >= visual.min_temperature: an "
            "inverted setpoint clamp range would pin every target to one bound",
            [CONF_VISUAL, CONF_MAX_TEMPERATURE],
        )
    return config


CONFIG_SCHEMA = cv.All(
    climate.climate_schema(VitoClimate)
    .extend(
        {
            cv.GenerateID(CONF_VITOHOME_ID): cv.use_id(VitoHomeComponent),
            cv.Required(CONF_TARGET_ADDRESS): cv.hex_uint16_t,
            cv.Optional(CONF_OPERATING_MODE): OPERATING_MODE_SCHEMA,
            cv.Optional(CONF_UPDATE_INTERVAL): cv.update_interval,
            # GWG access mode for the SETPOINT channel (target_address). The
            # Betriebsart channel has its own under operating_mode. GWG-only;
            # rejected under other protocols in _final_validate (__init__.py).
            cv.Optional(CONF_ACCESS): cv.enum(GWG_ACCESS_MODES, lower=True),
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _validate_setpoint_range,
)


def _emit_preset_table(config, presets):
    """Emit the preset table as static arrays in .rodata; return (table, count).

    One `static const uint8_t <id>_preset<N>_read[]` per preset, then a single
    `static const VitoClimatePreset <id>_presets[]` referencing them. Replaces
    the per-preset add_preset() run, which cost two heap blocks per preset (a
    std::string name and a std::vector<uint8_t> read list) plus the presets_
    vector's own growth.
    """
    if not presets:
        return None, 0
    rows = []
    for n, preset in enumerate(presets):
        reads = list(preset[CONF_READ])
        if reads:
            read_id = ID(f"{config[CONF_ID]}_preset{n}_read", is_declaration=True, type=cg.uint8)
            read_arr = cg.static_const_array(read_id, cg.ArrayInitializer(*[int(v) for v in reads]))
        else:
            read_arr = cg.RawExpression("nullptr")
        # cv.enum yields an EnumValue whose str() is the YAML key ("heat"), not
        # the C++ enumerator. safe_exp() unwraps it to climate::CLIMATE_MODE_HEAT
        # -- interpolating the EnumValue directly emits an undeclared identifier
        # that only fails at compile time.
        mode = cg.safe_exp(preset[CONF_MODE])
        rows.append(
            cg.RawExpression(
                f"{{{cpp_string_literal(preset[CONF_NAME])}, {int(preset[CONF_WRITE])}, {read_arr}, {len(reads)}, {mode}}}"
            )
        )
    table_id = ID(f"{config[CONF_ID]}_presets", is_declaration=True, type=VitoClimatePreset)
    return cg.static_const_array(table_id, cg.ArrayInitializer(*rows, multiline=True)), len(rows)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_VITOHOME_ID])
    # 0 rather than None here: configure_setpoint/configure_mode take a uint32_t
    # poll interval, where 0 means "poll on every hub cycle".
    poll_ms = pop_poll_interval(config) or 0
    var = await climate.new_climate(config)
    await cg.register_component(var, config)

    # CONF_NAME is always populated after validation: ESPHome's
    # _entity_base_validator copies `id:` into it when `name:` is omitted (and
    # errors out if neither is given), so no fallback is reachable here. Matches
    # the other eight platforms, which all index it directly.
    name = config[CONF_NAME]

    # Setpoint clamp range from the standard visual block (also the HA gauge).
    visual = config.get(CONF_VISUAL, {})
    sp_min = int(visual.get(CONF_MIN_TEMPERATURE, 3))
    sp_max = int(visual.get(CONF_MAX_TEMPERATURE, 37))
    cg.add(var.set_setpoint_range(sp_min, sp_max))

    # Setpoint channel: read == write at target_address, one integer degC byte.
    cg.add(var.configure_setpoint(parent, datapoint_expression(name, config[CONF_TARGET_ADDRESS], 1), poll_ms))
    if CONF_ACCESS in config:
        cg.add(var.set_setpoint_access(config[CONF_ACCESS]))

    # Operating-mode channel (optional): read state_address, write address.
    if CONF_OPERATING_MODE in config:
        om = config[CONF_OPERATING_MODE]
        write_addr = om[CONF_ADDRESS]
        read_addr = om.get(CONF_STATE_ADDRESS, write_addr)
        cg.add(var.configure_mode(parent, datapoint_expression(name, read_addr, 1), om[CONF_READ_BACK], poll_ms))
        if CONF_ACCESS in om:
            cg.add(var.set_mode_access(om[CONF_ACCESS]))
        if read_addr != write_addr:
            cg.add(var.set_mode_write_datapoint(datapoint_expression(name, write_addr, 1)))
        preset_table, preset_count = _emit_preset_table(config, om[CONF_PRESETS])
        if preset_count:
            cg.add(var.set_presets(preset_table, preset_count))
