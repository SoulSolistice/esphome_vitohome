"""Unit tests for the vitohome Python config layer.

Run under the ESPHome venv so the component's ``import esphome.codegen`` etc.
resolve::

    esphome-venv/bin/python -m pytest tests/unit -q

These cover the parts that turn YAML into the values handed to the C++ runtime:
the converter registry, the signed-resolution rule, and the cross-checks that
must fail at ``esphome config`` time rather than as a wrong/garbage write.
"""

import os
import sys

import pytest

# Make the repo's `components/` importable as a namespace package.
_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

import esphome.config_validation as cv  # noqa: E402

from components.vitohome import (  # noqa: E402
    CONF_CONVERTER,
    CONF_LENGTH,
    CONF_SIGNED,
    CONVERTERS,
    converter_default_signed,
    converter_scale,
    cpp_string_literal,
    datapoint_expression,
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
    """Replicate number.py::_validate_encodable_range's core decision."""
    scale = converter_scale(converter)
    is_signed = (
        signed_override if signed_override is not None else converter_default_signed(converter)
    )
    for value in (min_v, max_v):
        raw = round(value / scale)
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


# --- C++ literal / datapoint expression helpers ----------------------------


def test_cpp_string_literal_escapes():
    assert cpp_string_literal("plain") == '"plain"'
    assert cpp_string_literal('a"b') == '"a\\"b"'
    assert cpp_string_literal("a\\b") == '"a\\\\b"'


def test_datapoint_expression_uses_noconv_and_hex():
    expr = str(datapoint_expression('Kessel "K"', 0x0802, 2))
    assert "VitoWiFi::Datapoint(" in expr
    assert "VitoWiFi::noconv" in expr  # always bypass the library converter
    assert "0x0802" in expr
    assert '\\"K\\"' in expr  # name escaping survived
