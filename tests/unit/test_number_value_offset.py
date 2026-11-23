"""`value_offset` and `scale` on a writable number: value = raw * scale + value_offset.

GWG coding (05) is a 4-bit field 0..15 that means 35..110 degC (Vitosoft
MultOffset, factor 5, offset 35): `converter: mult5`, `value_offset: 35`. The
config check subtracts the offset before it range-checks the raw value, as
VitoNumber::control() does before encoding. `scale` takes a factor no
converter preset carries (K92 Nachstellzeit, raw * 3 s).

Also pinned: a whole-byte datapoint that shares its byte with bit fields
(Vitosoft's "...Byte" rows) is emitted read-only.

    python -m pytest tests/unit/test_number_value_offset.py -q
"""

import itertools
import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
for _p in (_REPO_ROOT, os.path.join(_REPO_ROOT, "scripts")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import esphome.config_validation as cv  # noqa: E402
from esphome.core import CORE  # noqa: E402
from vitocatalog.constants import PROTO_GWG  # noqa: E402
from vitocatalog.loader import Catalog  # noqa: E402
from vitocatalog.models import Event  # noqa: E402
from vitocatalog.renderer import generate  # noqa: E402

from components.vitohome.number import CONFIG_SCHEMA as NUMBER_SCHEMA  # noqa: E402


@pytest.fixture(autouse=True)
def _fresh_core():
    CORE.reset()
    yield
    CORE.reset()


_SEQ = itertools.count()


def _k05(**extra):
    cfg = {
        "name": f"K05 {next(_SEQ)}",
        "address": 0x0001,
        "length": 1,
        "bit_mask": 0x0F,
        "converter": "mult5",
        "value_offset": 35,
        "min_value": 35,
        "max_value": 110,
        "step": 5,
    }
    cfg.update(extra)
    return cfg


def test_offset_bounds_fit_the_field():
    cfg = NUMBER_SCHEMA(_k05())
    assert cfg["value_offset"] == 35


def test_value_offset_defaults_to_zero():
    cfg = NUMBER_SCHEMA(_k05(value_offset=0, min_value=0, max_value=75))
    assert cfg["value_offset"] == 0


@pytest.mark.parametrize("bound", [{"max_value": 115}, {"min_value": 30}])
def test_a_bound_outside_the_field_after_the_offset_is_refused(bound):
    with pytest.raises(cv.Invalid, match="value_offset 35"):
        NUMBER_SCHEMA(_k05(**bound))


def test_without_the_offset_the_display_bounds_do_not_fit():
    with pytest.raises(cv.Invalid, match="does not fit bit_mask"):
        NUMBER_SCHEMA(_k05(value_offset=0))


def test_a_whole_byte_offset_number():
    # (85) Verschiebung SPR 30L unten: raw * 0.5 - 16, -16..16 K -> raw 0..64.
    cfg = NUMBER_SCHEMA(
        {
            "name": f"K85 {next(_SEQ)}",
            "address": 0x001F,
            "converter": "div2",
            "value_offset": -16,
            "signed": False,
            "min_value": -16,
            "max_value": 16,
            "step": 0.5,
        }
    )
    assert cfg["value_offset"] == -16


# --- scale: a factor no converter preset carries --------------------------------


def _k92(**extra):
    # K92 Nachstellzeit: raw * 3 s, 0..765 step 3 -> raw 0..255.
    cfg = {"name": f"K92 {next(_SEQ)}", "address": 0x8712, "scale": 3, "min_value": 0, "max_value": 765, "step": 3}
    cfg.update(extra)
    return cfg


def test_scale_alone():
    assert NUMBER_SCHEMA(_k92())["scale"] == 3


def test_scale_with_an_offset_on_a_bit_field():
    cfg = NUMBER_SCHEMA(_k92(bit_mask=0x0F, value_offset=6, min_value=6, max_value=51))
    assert (cfg["scale"], cfg["value_offset"]) == (3, 6)


def test_a_two_byte_fractional_scale():
    cfg = NUMBER_SCHEMA(_k92(length=2, scale=0.015625, min_value=8, max_value=65, step=1))
    assert cfg["scale"] == 0.015625


def test_scale_beside_a_preset_is_refused():
    with pytest.raises(cv.Invalid, match="cannot be combined with converter 'mult5'"):
        NUMBER_SCHEMA(_k92(converter="mult5"))


@pytest.mark.parametrize("scale", [0, -3])
def test_scale_must_be_positive(scale):
    with pytest.raises(cv.Invalid):
        NUMBER_SCHEMA(_k92(scale=scale))


def test_a_bound_beyond_the_byte_names_the_scale():
    with pytest.raises(cv.Invalid, match="scale 3"):
        NUMBER_SCHEMA(_k92(max_value=768))


@pytest.mark.parametrize("bound", [{"step": 5}, {"min_value": 1}])
def test_with_scale_values_between_raw_steps_are_refused(bound):
    with pytest.raises(cv.Invalid, match="not a whole number of raw steps"):
        NUMBER_SCHEMA(_k92(**bound))


def test_with_a_preset_values_between_raw_steps_only_warn(caplog):
    cfg = NUMBER_SCHEMA(_k05(step=2.5))
    assert cfg["step"] == 2.5
    assert "not a whole number of raw steps" in caplog.text


# --- a whole byte that holds bit fields is read-only ---------------------------


def _ev(eid, tech, **over):
    kw = {
        "id": eid,
        "name": tech,
        "tech": tech,
        "token": f"{tech}~0x0005",
        "address": 0x0005,
        "conversion": "NoConversion",
        "access_type": 3,
        "block_length": 1,
        "byte_length": 1,
        "byte_position": 0,
        "bit_length": 0,
        "bit_position": 0,
        "lower": "0",
        "upper": "255",
        "stepping": "1",
        "fc_read": "EEPROM_READ",
        "fc_write": "EEPROM_WRITE",
    }
    kw.update(over)
    return Event(**kw)


def _render(events):
    cat = Catalog(
        types={"GWG_T": "dp"},
        dp_name={"dp": "GWG T"},
        links={"dp": [e.id for e in events]},
        raw_events={},
        vlinks={},
        raw_values={},
        access={},
        ident_rows=[],
        textmap={},
    )
    cat._events = {e.id: e for e in events}
    return generate(cat, "GWG_T", "full", None, None, protocol=PROTO_GWG)


def test_whole_byte_over_bit_fields_is_read_only():
    whole = _ev("1", "GWG_K11Byte")
    bit = _ev("2", "GWG_K11", bit_length=1, bit_position=6)
    out = _render([whole, bit])
    sensors = out.split("\nsensor:\n")[1].split("\nnumber:\n")[0]
    assert "id: GWG_K11Byte" in sensors
    assert "which a whole write would overwrite -> read-only" in sensors


def test_whole_byte_alone_stays_writable():
    out = _render([_ev("1", "GWG_K11Byte")])
    assert "\nnumber:\n" in out
    assert "GWG_K11Byte" in out.split("\nnumber:\n")[1]


def test_bit_fields_in_another_mode_do_not_count():
    whole = _ev("1", "GWG_K11Byte")
    bit = _ev("2", "GWG_P05", bit_length=1, bit_position=6, fc_read="Port_READ", fc_write="", access_type=1)
    out = _render([whole, bit])
    assert "GWG_K11Byte" in out.split("\nnumber:\n")[1]
