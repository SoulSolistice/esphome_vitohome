"""Raw encoding inferred from borders + fractional stepping (gen_catalog).

Vitosoft declares ``Conversion = NoConversion`` on a few datapoints whose
borders move in fractional steps -- impossible for a plain raw value. The
motivating row is GWG_BEMK55Raumkorrekturwert (EEPROM 0x0016, 1 byte,
-5..5 / 0.125): the generator used to emit it as a writable ``signed: true``
noconv number, so HA showed the raw 40 and a write of 1 stored raw 0x01.
The byte reads 0x28 on hardware (issue #21), which the affine reading
``raw * 0.125 - 5`` turns into 0.0 K, the factory default.

The inference is unverified until a hardware write confirms it, so every
inferred entity is a read-only sensor. These tests pin the rule, its
exclusions, and that the stale ConversionFactor/Offset columns are ignored.

    python -m pytest tests/unit/test_gen_catalog_bounds_scale.py -q
"""

import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_SCRIPTS = os.path.join(_REPO_ROOT, "scripts")
for _p in (_SCRIPTS, _REPO_ROOT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from vitocatalog.constants import PROTO_GWG, PROTO_P300  # noqa: E402
from vitocatalog.emitter import emit_entity  # noqa: E402
from vitocatalog.models import Event  # noqa: E402
from vitocatalog.transforms import _scale_from_bounds  # noqa: E402


def _raumkorrekturwert(**over):
    """Shaped like the export row (DataPointDefinitionVersion 0.0.26.4683,
    token GWG_VBEM_21/GWG_VBEM_36), including its stale 5/35 factor pair."""
    kw = {
        "id": "t",
        "name": "GWG BEMK55Raumkorrekturwert",
        "tech": "GWG_BEMK55Raumkorrekturwert",
        "address": 0x0016,
        "conversion": "NoConversion",
        "access_type": 3,
        "block_length": 1,
        "byte_length": 1,
        "byte_position": 0,
        "bit_length": 0,
        "bit_position": 0,
        "lower": "-5",
        "upper": "5",
        "stepping": "0.125",
        "unit": "°C",
        "fc_read": "EEPROM_READ",
        "fc_write": "EEPROM_WRITE",
        "conv_factor": 5.0,
        "conv_offset": 35.0,
    }
    kw.update(over)
    return Event(**kw)


def _filters(lines):
    return [
        (name, float(s.split(":", 1)[1]))
        for s in (ln.strip() for ln in lines)
        for name in ("multiply", "offset")
        if s.startswith(f"- {name}:")
    ]


def _apply(filters, raw):
    v = float(raw)
    for name, arg in filters:
        v = v * arg if name == "multiply" else v + arg
    return v


# --- the motivating row -------------------------------------------------------


def test_raumkorrekturwert_infers_affine_on_gwg():
    bs = _scale_from_bounds(_raumkorrekturwert(), PROTO_GWG)
    assert bs is not None
    assert (bs.kind, bs.scale, bs.offset, bs.raw_lo, bs.raw_hi) == ("affine", 0.125, -5.0, 0, 80)


def test_raumkorrekturwert_emits_read_only_unsigned_sensor_with_filters():
    platform, lines = emit_entity(_raumkorrekturwert(), "full", PROTO_GWG)
    text = "\n".join(lines)
    assert platform == "sensor"
    assert "  converter: noconv" in lines
    assert "signed: true" not in text  # the raw byte is 0..80, not two's complement
    assert "min_value" not in text and "step:" not in text
    assert _filters(lines) == [("multiply", 0.125), ("offset", -5.0)]
    assert "UNVERIFIED" in text
    # the stale factor/offset pair is named as ignored, never applied
    assert "factor/offset (5, 35) ignored" in text
    assert "raw * 5 + 35" not in text


@pytest.mark.parametrize(
    "raw,value",
    [
        (0x28, 0.0),  # observed on hardware (issue #21): factory default 0 K
        (0x29, 0.125),  # write test v4.4 P4 target: +0.125 K on the display
        (0x00, -5.0),
        (0x50, 5.0),
    ],
)
def test_raumkorrekturwert_filters_decode(raw, value):
    _, lines = emit_entity(_raumkorrekturwert(), "full", PROTO_GWG)
    assert _apply(_filters(lines), raw) == pytest.approx(value)


def test_negative_lower_outside_gwg_is_ambiguous_raw():
    # Same borders on P300 (cf. K70_NRP_KonfiOffsetAussentemperatur 0x7770):
    # signed raw * step and raw * step + lower both fit; nothing decides.
    ev = _raumkorrekturwert(fc_read="Virtual_READ", fc_write="Virtual_WRITE")
    assert _scale_from_bounds(ev, PROTO_P300).kind == "ambiguous"
    platform, lines = emit_entity(ev, "full", PROTO_P300)
    assert platform == "sensor"
    assert _filters(lines) == []
    assert "signed: true" not in "\n".join(lines)
    assert "Encoding unknown" in "\n".join(lines)


# --- lower >= 0: pure scale ---------------------------------------------------


def test_positive_lower_is_pure_scale():
    # SR13_K17_KonfiKTReglerverstaerkung: 0.1..9.9 / 0.1 -> raw 1..99, no offset
    ev = _raumkorrekturwert(lower="0.1", upper="9.9", stepping="0.1", conv_factor=None, conv_offset=None)
    bs = _scale_from_bounds(ev, PROTO_P300)
    assert (bs.kind, bs.offset, bs.raw_lo, bs.raw_hi) == ("scale", 0.0, 1, 99)
    platform, lines = emit_entity(ev, "full", PROTO_P300)
    assert platform == "sensor"
    assert _filters(lines) == [("multiply", 0.1)]


# --- exclusions ---------------------------------------------------------------


@pytest.mark.parametrize(
    "over",
    [
        # single bits are booleans, never scaled (multi-bit fields: test_gen_catalog_multibit.py)
        {"lower": "0", "upper": "1", "stepping": "0.1", "bit_length": 1, "bit_position": 4},
        # raw companion of a converted sibling (GWG_TimerNachlaufRohwert 0x003A)
        {
            "tech": "GWG_TimerNachlaufRohwert",
            "name": "GWG TimerNachlaufRohwert",
            "lower": "0",
            "upper": "100",
            "stepping": "0.01",
            "byte_length": 2,
            "block_length": 2,
        },
        # integral stepping: consistent with a plain raw value
        {"lower": "-13", "upper": "40", "stepping": "1"},
        # raw range does not fit the byte width (0..1000 in 1 byte)
        {"lower": "0", "upper": "100", "stepping": "0.1"},
        # borders not whole steps
        {"lower": "0.05", "upper": "1", "stepping": "0.1"},
        # a declared conversion wins
        {"conversion": "MultOffset"},
        {"conversion": "Div2"},
        # missing borders
        {"lower": "", "upper": "", "stepping": ""},
    ],
)
def test_rule_does_not_apply(over):
    assert _scale_from_bounds(_raumkorrekturwert(**over), PROTO_GWG) is None


def test_integral_stepping_row_is_unchanged():
    # GWG_K45MaximaldrehzahlHKP-shaped: 1..100 % step 1 with the same stale
    # 5/35 pair -- outside the rule, still a writable number.
    ev = _raumkorrekturwert(lower="1", upper="100", stepping="1", unit="%")
    platform, lines = emit_entity(ev, "full", PROTO_GWG)
    assert platform == "number"
    assert _filters(lines) == []
