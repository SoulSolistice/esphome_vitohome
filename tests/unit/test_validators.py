"""Unit tests for the vitohome Python config layer.

Run under the ESPHome venv so the component's ``import esphome.codegen`` etc.
resolve::

    esphome-venv/bin/python -m pytest tests/unit -q

These cover the parts that turn YAML into the values handed to the C++ runtime:
the converter registry, the signed-resolution rule, and the cross-checks that
must fail at ``esphome config`` time rather than as a wrong/garbage write.
"""

import os
import pathlib
import re
import sys

import pytest

# Make the repo's `components/` importable as a namespace package.
_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

import esphome.config_validation as cv  # noqa: E402

from components.vitohome import (  # noqa: E402
    CONF_CLOCK_ADDRESS,
    CONF_CONVERTER,
    CONF_LENGTH,
    CONF_RAW_QUEUE_SIZE,
    CONF_SIGNED,
    CONF_TIME_ID,
    CONFIG_SCHEMA,
    CONVERTERS,
    TIME_SYNC_SCHEMA,
    converter_big_endian,
    converter_default_signed,
    converter_scale,
    cpp_string_literal,
    datapoint_expression,
    llround,
    raw_fits,
    resolve_signed,
    validate_converter_length,
)

# --- converter registry ----------------------------------------------------


def test_converter_scales():
    assert converter_scale("noconv") == 1.0
    assert converter_scale("div2") == 0.5
    assert converter_scale("div10") == pytest.approx(0.1)
    assert converter_scale("div100") == pytest.approx(0.01)
    assert converter_scale("div1000") == pytest.approx(0.001)
    assert converter_scale("sec2hour") == pytest.approx(1.0 / 3600.0)
    assert converter_scale("mult2") == 2.0
    assert converter_scale("mult100") == 100.0


def test_signed_defaults():
    # Only the Vitosoft "DivN" temperature-style conversions are signed.
    assert converter_default_signed("div2") is True
    assert converter_default_signed("div10") is True
    for name in ("noconv", "div100", "div1000", "sec2hour", "mult2", "mult10"):
        assert converter_default_signed(name) is False


def test_sec2hour_not_encodable():
    # A counter must never back a writable entity.
    assert CONVERTERS["sec2hour"].encodable is False
    # ... while the scaled presets are encodable.
    for name in ("noconv", "div2", "div10", "div100", "div1000", "mult2"):
        assert CONVERTERS[name].encodable is True


def test_converter_lengths_are_sane():
    for name, conv in CONVERTERS.items():
        assert conv.lengths, f"{name} has no lengths"
        assert all(1 <= n <= 4 for n in conv.lengths), name


# --- raw_fits (shared by number + select range checks) ---------------------


@pytest.mark.parametrize(
    "raw,length,signed,ok",
    [
        (0, 1, False, True),
        (255, 1, False, True),
        (256, 1, False, False),
        (-1, 1, False, False),
        (127, 1, True, True),
        (-128, 1, True, True),
        (128, 1, True, False),
        (-129, 1, True, False),
        (65535, 2, False, True),
        (65536, 2, False, False),
        (32767, 2, True, True),
        (-32768, 2, True, True),
        (32768, 2, True, False),
        (0xFFFFFFFF, 4, False, True),
        (0x100000000, 4, False, False),
    ],
)
def test_raw_fits(raw, length, signed, ok):
    assert raw_fits(raw, length, signed) is ok


# --- resolve_signed ---------------------------------------------------------


def test_resolve_signed_uses_converter_default():
    assert resolve_signed({CONF_CONVERTER: "div10", CONF_SIGNED: None}) is True
    assert resolve_signed({CONF_CONVERTER: "noconv", CONF_SIGNED: None}) is False


def test_resolve_signed_explicit_override_wins():
    # noconv defaults unsigned, but an explicit signed:true (the Niveau case)
    # must win.
    assert resolve_signed({CONF_CONVERTER: "noconv", CONF_SIGNED: True}) is True
    # And an explicit false overrides a signed-by-default converter.
    assert resolve_signed({CONF_CONVERTER: "div10", CONF_SIGNED: False}) is False


# --- validate_converter_length ---------------------------------------------


def test_converter_length_accepts_valid():
    cfg = {CONF_CONVERTER: "div10", CONF_LENGTH: 2}
    assert validate_converter_length(cfg) is cfg


def test_converter_length_rejects_invalid():
    # sec2hour is 4-byte only.
    with pytest.raises(cv.Invalid):
        validate_converter_length({CONF_CONVERTER: "sec2hour", CONF_LENGTH: 2})
    # div1000 has no 1-byte form.
    with pytest.raises(cv.Invalid):
        validate_converter_length({CONF_CONVERTER: "div1000", CONF_LENGTH: 1})


# --- the number encodable-range check (mirrors number.py / decode.h) -------


def _encodable_bounds(min_v, max_v, converter, length, signed_override=None):
    """Replicate number.py::_validate_encodable_range's core decision.

    Uses ``llround`` (half away from zero), exactly like the production
    validator and ``decode.h::encode_scaled``. Python's built-in ``round()``
    is banker's rounding and silently diverges at negative half-steps --
    ``test_encodable_range_negative_half_step_rejected`` pins the difference.
    """
    scale = converter_scale(converter)
    is_signed = signed_override if signed_override is not None else converter_default_signed(converter)
    for value in (min_v, max_v):
        raw = llround(value / scale)
        if not raw_fits(raw, length, is_signed):
            return False
    return min_v <= max_v


def test_encodable_range_dhw_setpoint_ok():
    # 10..60 C, noconv, 1 byte -> raw 10..60, fits.
    assert _encodable_bounds(10, 60, "noconv", 1)


def test_encodable_range_slope_div10_ok():
    # 0.2..3.5 with div10 -> raw 2..35, fits 1 byte.
    assert _encodable_bounds(0.2, 3.5, "div10", 1)


def test_encodable_range_niveau_signed_ok():
    # -13..40 signed noconv -> fits int8.
    assert _encodable_bounds(-13, 40, "noconv", 1, signed_override=True)


def test_encodable_range_rejects_overflow():
    # 0..300 with noconv 1 byte -> raw 300 > 255.
    assert not _encodable_bounds(0, 300, "noconv", 1)
    # -13..40 with UNsigned noconv -> raw -13 < 0.
    assert not _encodable_bounds(-13, 40, "noconv", 1, signed_override=False)


def test_llround_half_away_from_zero():
    """llround must match C++ std::llround, not Python's banker's round().

    The two agree everywhere except exact half-steps; the negative half-step
    is where a config-time check using round() would accept a bound the C++
    encode path rejects at runtime.
    """
    assert llround(128.5) == 129
    assert llround(-128.5) == -129
    assert llround(0.5) == 1
    assert llround(-0.5) == -1
    assert llround(2.4) == 2
    assert llround(-2.4) == -2
    # The divergence this guards against (Python semantics, for contrast):
    assert round(-128.5) == -128


def test_encodable_range_negative_half_step_rejected():
    # -64.25 with div2 (scale 0.5, signed) -> raw_d exactly -128.5 (both
    # operands and the quotient are exact in binary). llround gives -129,
    # which does NOT fit int8 -- the bound must be rejected. Under banker's
    # round() the raw would be -128 and the bad bound would silently pass.
    assert not _encodable_bounds(-64.25, 0, "div2", 1)
    # One step inside the boundary is fine: -64.0 -> raw -128 == int8 min.
    assert _encodable_bounds(-64.0, 0, "div2", 1)


# --- rotatebytes (big-endian, read-only) ------------------------------------


def test_rotatebytes_registry_shape():
    # RotateBytes is the big-endian 2-byte coding-value preset: raw value,
    # unsigned, exactly two bytes, read-only (no defined inverse).
    conv = CONVERTERS["rotatebytes"]
    assert conv.scale == 1.0
    assert conv.default_signed is False
    assert conv.lengths == (2,)
    assert conv.encodable is False
    assert conv.big_endian is True
    assert converter_big_endian("rotatebytes") is True
    # Every other byte-order-sensitive path in the component is little-endian.
    assert converter_big_endian("noconv") is False


def test_rotatebytes_length_validation():
    cfg = {CONF_CONVERTER: "rotatebytes", CONF_LENGTH: 2}
    assert validate_converter_length(cfg) is cfg
    for bad_length in (1, 4):
        with pytest.raises(cv.Invalid):
            validate_converter_length({CONF_CONVERTER: "rotatebytes", CONF_LENGTH: bad_length})


# --- the hub raw_queue_size default -----------------------------------------
#
# There is deliberately no raw_queue_size/time_id cross-check any more. Clock
# sync used to ride the raw lane, so a zero-capacity lane silently disabled it
# and a validator had to reject that combination. The clock is now a VitoClock
# entity on the ordinary read/write lanes, so the raw lane serves only the
# interactive scan console and 0 is a perfectly good value for it -- including
# alongside time_id.


def _raw_queue_size_schema_key():
    """Return the cv.Optional key object for raw_queue_size from CONFIG_SCHEMA."""
    inner = CONFIG_SCHEMA.validators[0]
    for key in inner.schema:
        if str(key) == CONF_RAW_QUEUE_SIZE:
            return key
    raise AssertionError(f"{CONF_RAW_QUEUE_SIZE} not found in CONFIG_SCHEMA")


def test_raw_queue_size_defaults_to_zero():
    """The scan console is opt-in; nobody pays for a lane they do not use.

    This is the whole point of moving clock sync off the raw lane. If the
    default drifts back above 0, every config -- including an ESP8266 build with
    ~40 KiB of heap -- silently starts allocating a debug tool's queue again.
    """
    assert _raw_queue_size_schema_key().default() == 0


def test_no_raw_queue_size_cross_check_remains():
    """time_id + raw_queue_size: 0 must be accepted -- it is now the DEFAULT.

    The old _validate_raw_queue_size rejected exactly this pair, because clock
    sync rode the raw lane. It no longer does, so the rule is gone; if it were
    reintroduced it would reject the single most common config (time sync on,
    no scan console). Pinned two ways: the helper must not exist any more, and
    the config-level validators that DO remain must accept the pair.
    """
    import components.vitohome as vh

    assert not hasattr(vh, "_validate_raw_queue_size")

    cfg = {CONF_TIME_ID: "my_time", CONF_RAW_QUEUE_SIZE: 0}
    for validator in CONFIG_SCHEMA.validators[1:]:
        assert validator(cfg) is cfg


def test_raw_queue_size_range():
    key = _raw_queue_size_schema_key()
    validator = CONFIG_SCHEMA.validators[0].schema[key]
    assert validator(0) == 0
    assert validator(1024) == 1024
    with pytest.raises(cv.Invalid):
        validator(-1)
    with pytest.raises(cv.Invalid):
        validator(1025)


def test_raw_queue_size_cpp_and_python_defaults_agree():
    """RAW_QUEUE_DEFAULT in vitohome.h must match the schema default.

    to_code() always emits set_raw_queue_capacity(), so the Python default is
    the one that actually takes effect and the C++ initializer is a fallback
    for direct C++ users. They are two independent literals; nothing but this
    test stops them drifting apart and making the header lie about behavior.
    """
    header = (pathlib.Path(__file__).resolve().parents[2] / "components" / "vitohome" / "vitohome.h").read_text()
    match = re.search(r"constexpr\s+std::size_t\s+RAW_QUEUE_DEFAULT\s*=\s*(\d+)\s*;", header)
    assert match is not None, "RAW_QUEUE_DEFAULT not found in vitohome.h"
    assert int(match.group(1)) == _raw_queue_size_schema_key().default()


# --- the clock address is a device property, not a constant -----------------


def test_clock_address_defaults_to_nrf_vitotronic():
    """0x088E is the NRF/Vitotronic clock, and the reference unit's.

    Source-confirmed against the Vitosoft DPDefinitions.xml link tables:
    NRF_Uhrzeit~0x088E. Hardware-confirmed on a Vitodens 300-W (B3HA).
    """
    assert TIME_SYNC_SCHEMA({})[CONF_CLOCK_ADDRESS] == 0x088E


def test_clock_address_accepts_the_wpr_heat_pump_address():
    """WPR heat-pump controllers put the same 8-byte DateTimeBCD at 0x08E0.

    Tokens V200WO1A, VBC700_AW, VBC700_BW_WW, VBC702_AW, VBC702_S,
    CU401B_A/G/S. This option is the ONLY thing that lets those devices sync --
    without it the component reads 8 bytes from 0x088E, which on a heat pump is
    a different datapoint entirely, and then blind-writes a BCD timestamp to it.
    """
    assert TIME_SYNC_SCHEMA({CONF_CLOCK_ADDRESS: 0x08E0})[CONF_CLOCK_ADDRESS] == 0x08E0


def test_clock_address_rejects_out_of_range():
    with pytest.raises(cv.Invalid):
        TIME_SYNC_SCHEMA({CONF_CLOCK_ADDRESS: 0x10000})


def test_clock_address_cpp_and_python_defaults_agree():
    """CLOCK_ADDRESS_DEFAULT in vito_clock.h must match the schema default.

    Same class of trap as RAW_QUEUE_DEFAULT: two independent literals, and the
    Python one wins because to_code() always emits set_time_sync(). Nothing else
    stops the header lying about behaviour.
    """
    header = (pathlib.Path(__file__).resolve().parents[2] / "components" / "vitohome" / "vito_clock.h").read_text()
    match = re.search(r"constexpr\s+uint16_t\s+CLOCK_ADDRESS_DEFAULT\s*=\s*(0x[0-9A-Fa-f]+)\s*;", header)
    assert match is not None, "CLOCK_ADDRESS_DEFAULT not found in vito_clock.h"
    assert int(match.group(1), 16) == TIME_SYNC_SCHEMA({})[CONF_CLOCK_ADDRESS]


def test_clock_len_is_fixed_at_eight():
    """The address moves per device; the LENGTH must not.

    Both DateTimeBCD variants are 8 bytes, and 8 is exactly
    VitoEntityBase::write_buf_ -- which is what lets the clock ride the ordinary
    entity write path with no growth. If CLOCK_LEN ever exceeds write_buf_,
    set_write_payload_() starts rejecting the clock payload at runtime.
    """
    vito_clock = (pathlib.Path(__file__).resolve().parents[2] / "components" / "vitohome" / "vito_clock.h").read_text()
    entity = (pathlib.Path(__file__).resolve().parents[2] / "components" / "vitohome" / "vito_entity.h").read_text()

    clock_len = re.search(r"constexpr\s+uint8_t\s+CLOCK_LEN\s*=\s*(\d+)\s*;", vito_clock)
    write_buf = re.search(r"write_buf_\[(\d+)\]", entity)
    assert clock_len is not None and write_buf is not None
    assert int(clock_len.group(1)) == 8
    assert int(clock_len.group(1)) <= int(write_buf.group(1))


# --- C++ literal / datapoint expression helpers ----------------------------


def test_cpp_string_literal_escapes():
    assert cpp_string_literal("plain") == '"plain"'
    assert cpp_string_literal('a"b') == '"a\\"b"'
    assert cpp_string_literal("a\\b") == '"a\\\\b"'


def test_datapoint_expression_uses_noconv_and_hex():
    expr = str(datapoint_expression('Kessel "K"', 0x0802, 2))
    assert "esphome::vitohome::optolink::Datapoint(" in expr
    assert "esphome::vitohome::optolink::noconv" in expr  # always bypass the engine converter
    assert "0x0802" in expr
    assert '\\"K\\"' in expr  # name escaping survived
