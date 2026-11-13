"""`bit_mask` on sensor, text_sensor enum, number, select and switch.

A bit field is a run of adjacent bits inside ONE byte, named by its mask. These
tests pin the config-time rules that keep a bit-field write from corrupting the
rest of its byte (guidelines E2: anything that would corrupt the wire is
rejected at `esphome config` time):

* the mask is contiguous and non-zero (mirrors decode.h::bit_mask_contiguous);
* the field is one byte wide;
* values (number min/max, select options, switch on/off, enum options) fit the
  field, never the byte;
* a writable bit field reads and writes the same register -- no state_address,
  no byte_offset -- because the write is a read-modify-write of that byte;
* signed is rejected.

    python -m pytest tests/unit/test_bit_mask.py -q
"""

import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

import esphome.config_validation as cv  # noqa: E402
from esphome.core import CORE  # noqa: E402

from components.vitohome import (  # noqa: E402
    BIT_MASK_SCHEMA,
    CONF_BIT_MASK,
    binary_sensor as bs_mod,  # noqa: E402
    bit_field_max,
    number as number_mod,  # noqa: E402
    select as select_mod,  # noqa: E402
    sensor as sensor_mod,  # noqa: E402
    switch as switch_mod,  # noqa: E402
    text_sensor as ts_mod,  # noqa: E402
)


@pytest.fixture(autouse=True)
def _fresh_core():
    # Entity names are registered in CORE for uniqueness; reset between cases.
    CORE.reset()
    yield
    CORE.reset()


_SEQ = iter(range(100000))


def _name():
    return f"bitfield {next(_SEQ)}"


# --- the mask itself -----------------------------------------------------------


def _reference_contiguous(m: int) -> bool:
    runs, in_run = 0, False
    for b in range(8):
        on = bool((m >> b) & 1)
        if on and not in_run:
            runs += 1
        in_run = on
    return runs == 1


def test_mask_contiguity_matches_reference_for_all_256():
    # Python twin of test_decode.cpp's exhaustive contiguity check.
    accepted = 0
    for m in range(256):
        try:
            BIT_MASK_SCHEMA(m)
            ok = True
        except cv.Invalid:
            ok = False
        assert ok == _reference_contiguous(m), hex(m)
        accepted += ok
    assert accepted == 36


@pytest.mark.parametrize(
    "mask,top",
    [(0x01, 1), (0x08, 1), (0x0C, 3), (0x0F, 15), (0xF0, 15), (0x1E, 15), (0x1F, 31), (0xE0, 7), (0xFF, 255)],
)
def test_bit_field_max(mask, top):
    assert bit_field_max(mask) == top


def test_binary_sensor_uses_the_shared_key_and_keeps_any_mask():
    # binary_sensor's "any masked bit set" semantics are defined for any mask,
    # so any non-zero mask is valid, a gapped one included.
    assert bs_mod.CONF_BIT_MASK == CONF_BIT_MASK
    cfg = bs_mod.CONFIG_SCHEMA({"name": _name(), "address": 0x0005, "bit_mask": 0x0A})
    assert cfg["bit_mask"] == 0x0A


def test_binary_sensor_rejects_a_zero_mask():
    with pytest.raises(cv.Invalid, match="bit_mask must select at least one bit"):
        bs_mod.CONFIG_SCHEMA({"name": _name(), "address": 0x0005, "bit_mask": 0x00})


# --- read-only platforms --------------------------------------------------------


def test_sensor_bit_field_accepted_plain_and_extracted():
    cfg = sensor_mod.CONFIG_SCHEMA({"name": _name(), "address": 0x001E, "length": 1, "converter": "div10", "bit_mask": 0xF0})
    assert cfg[CONF_BIT_MASK] == 0xF0
    # nvoAlarm_Anlagennummer on VScotHO1_72: byte 14 of a 29-byte block, low nibble.
    cfg = sensor_mod.CONFIG_SCHEMA({"name": _name(), "address": 0xA132, "length": 29, "byte_offset": 14, "bit_mask": 0x0F})
    assert cfg["byte_offset"] == 14


@pytest.mark.parametrize(
    "extra,path",
    [
        ({"length": 2}, "bit_mask"),  # field wider than one byte
        ({"length": 4, "byte_offset": 0, "byte_length": 2}, "bit_mask"),
        ({"length": 1, "signed": True}, "signed"),
        ({"length": 1, "bit_mask": 0x0A}, None),  # gapped
        ({"length": 1, "bit_mask": 0x00}, None),  # empty
    ],
)
def test_sensor_bit_field_rejections(extra, path):
    conf = {"name": _name(), "address": 0x001E, "bit_mask": 0xF0}
    conf.update(extra)
    with pytest.raises(cv.Invalid) as e:
        sensor_mod.CONFIG_SCHEMA(conf)
    if path is not None:
        assert path in [str(p) for p in e.value.path]


def test_enum_bit_field_options_in_field_units():
    # SM1_Sensor_7_Status_GWG (VScotHO1_72, 0xCF90 byte 4, 7-bit field 0x7F).
    base = {
        "type": "enum",
        "address": 0xCF90,
        "length": 20,
        "byte_offset": 4,
        "bit_mask": 0x7F,
    }
    cfg = ts_mod.CONFIG_SCHEMA({**base, "name": _name(), "options": {0: "a", 1: "b", 2: "c"}})
    assert cfg[CONF_BIT_MASK] == 0x7F
    # The export also lists 128, which cannot fit seven bits: rejected, not
    # silently dead (a value outside the field can never be decoded).
    with pytest.raises(cv.Invalid, match="does not fit bit_mask"):
        ts_mod.CONFIG_SCHEMA({**base, "name": _name(), "options": {0: "a", 128: "x"}})


# --- writable platforms -----------------------------------------------------------


def _number(**kw):
    conf = {
        "name": _name(),
        "address": 0x001E,
        "length": 1,
        "converter": "div10",
        "bit_mask": 0xF0,
        "min_value": 0,
        "max_value": 1,
        "step": 0.1,
    }
    conf.update(kw)
    return number_mod.CONFIG_SCHEMA(conf)


def test_number_spr30_accepted():
    # GWG_BEMK89SchaltpunktSPR30Loben: high nibble of EEPROM 0x001E, 0..1 / 0.1.
    cfg = _number()
    assert cfg[CONF_BIT_MASK] == 0xF0


def test_number_range_is_checked_against_the_field_not_the_byte():
    # 2.0 -> raw 20, fits a byte but not a nibble.
    with pytest.raises(cv.Invalid, match="does not fit bit_mask"):
        _number(max_value=2)
    # 1.5 -> raw 15 is the nibble's top value: accepted.
    assert _number(max_value=1.5)["max_value"] == 1.5


@pytest.mark.parametrize(
    "extra,key",
    [
        ({"state_address": 0x001F}, "state_address"),
        ({"length": 2}, "bit_mask"),
        ({"signed": True, "min_value": 0}, "signed"),
    ],
)
def test_number_writable_shape_rules(extra, key):
    with pytest.raises(cv.Invalid) as e:
        _number(**extra)
    assert key in [str(p) for p in e.value.path]


def test_number_byte_offset_rejected_for_a_writable_bit_field():
    # byte_offset requires state_address on writables, so either rule may name
    # the problem; what matters is that it never validates.
    with pytest.raises(cv.Invalid):
        _number(length=2, byte_offset=0, state_address=0x001E)


def _select(**kw):
    conf = {
        "name": _name(),
        "address": 0x0005,
        "bit_mask": 0x08,
        "options": {0: "0 mit Nachlauf bis 10 Min.", 1: "1 ohne Nachlauf"},
    }
    conf.update(kw)
    return select_mod.CONFIG_SCHEMA(conf)


def test_select_k13_accepted():
    # GWG_K13Speichernachlauf: one flag of the eight at EEPROM 0x0005.
    assert _select()[CONF_BIT_MASK] == 0x08


def test_select_option_must_fit_the_field():
    with pytest.raises(cv.Invalid, match="does not fit bit_mask"):
        _select(options={0: "a", 2: "b"})  # 2 does not fit one bit


def test_select_nibble_options():
    # GWG_K08NeigungKessel: options 0..15 in the high nibble of 0x0002.
    cfg = _select(address=0x0002, bit_mask=0xF0, options={i: f"{i}" for i in range(16)})
    assert len(cfg["options"]) == 16


def test_select_state_address_rejected():
    with pytest.raises(cv.Invalid) as e:
        _select(state_address=0x0006)
    assert "state_address" in [str(p) for p in e.value.path]


def _switch(**kw):
    conf = {"name": _name(), "address": 0x0006, "bit_mask": 0x80}
    conf.update(kw)
    return switch_mod.CONFIG_SCHEMA(conf)


def test_switch_single_flag_accepted():
    # GWG_K27Ruecklaufsensor: bit 0x80 of EEPROM 0x0006.
    cfg = _switch()
    assert cfg[CONF_BIT_MASK] == 0x80
    assert cfg["on_value"] == 1 and cfg["off_value"] == 0


def test_switch_values_must_fit_the_field():
    with pytest.raises(cv.Invalid, match="does not fit bit_mask"):
        _switch(on_value=2)
    with pytest.raises(cv.Invalid, match="does not fit bit_mask"):
        _switch(bit_mask=0x30, on_values=[1, 4])  # 2-bit field: 4 does not fit


def test_switch_two_bit_field():
    # GWG_BEMK20FernbedienungAnalog: 2-bit field 0x30 at EEPROM 0x0013.
    cfg = _switch(address=0x0013, bit_mask=0x30, on_value=1, off_value=0, on_values=[1, 2, 3])
    assert cfg["on_values"] == [1, 2, 3]


def test_switch_state_address_rejected():
    with pytest.raises(cv.Invalid) as e:
        _switch(state_address=0x2303)
    assert "state_address" in [str(p) for p in e.value.path]


def test_writable_without_bit_mask_is_unchanged():
    # No bit_mask: the pre-existing shapes still validate (state_address split).
    cfg = switch_mod.CONFIG_SCHEMA({"name": _name(), "address": 0x2330, "state_address": 0x2303})
    assert CONF_BIT_MASK not in cfg
