"""Tests for P300-portable block-aligned byte extraction: the sensor schema's
byte_offset / byte_length, and gen_catalog emitting aligned reads for interior
fields instead of unaligned interior addresses (which NAK on P300)."""

import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
for _p in (_REPO_ROOT, os.path.join(_REPO_ROOT, "scripts")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import esphome.config_validation as cv  # noqa: E402
from esphome.core import CORE  # noqa: E402
import gen_catalog as gc  # noqa: E402

from components.vitohome import MAX_P300_READ_LENGTH  # noqa: E402
from components.vitohome.number import CONFIG_SCHEMA as NUMBER_SCHEMA  # noqa: E402
from components.vitohome.select import CONFIG_SCHEMA as SELECT_SCHEMA  # noqa: E402
from components.vitohome.sensor import CONFIG_SCHEMA as SENSOR_SCHEMA  # noqa: E402
from components.vitohome.switch import CONFIG_SCHEMA as SWITCH_SCHEMA  # noqa: E402
from components.vitohome.text_sensor import CONFIG_SCHEMA as TEXT_SENSOR_SCHEMA  # noqa: E402


@pytest.fixture(autouse=True)
def _fresh_core():
    CORE.reset()
    yield
    CORE.reset()


_SEQ = iter(range(10000))


def _name():
    return f"Extract {next(_SEQ)}"


def _base(**extra):
    cfg = {"name": _name(), "address": 0x7660, "length": 1, "converter": "noconv"}
    cfg.update(extra)
    return cfg


# --- schema: single-byte extract from a wide block --------------------------


def test_byte_offset_allows_block_read_wider_than_four():
    # length is a block read when byte_offset is present: 16 is fine (would be
    # rejected as a plain scalar length).
    cfg = SENSOR_SCHEMA(_base(length=16, byte_offset=9))
    assert cfg["length"] == 16
    assert cfg["byte_offset"] == 9


def test_plain_length_still_capped_at_four():
    with pytest.raises(cv.Invalid, match="between 1 and 4"):
        SENSOR_SCHEMA(_base(length=16))


def test_byte_offset_must_fit_the_block():
    with pytest.raises(cv.Invalid, match="must be <= length"):
        SENSOR_SCHEMA(_base(length=2, byte_offset=2))


def test_block_read_cannot_exceed_p300_cap():
    with pytest.raises(cv.Invalid, match="block read"):
        SENSOR_SCHEMA(_base(length=MAX_P300_READ_LENGTH + 1, byte_offset=0))


# --- schema: multi-byte field extract ---------------------------------------


def test_byte_length_extracts_a_multi_byte_field():
    cfg = SENSOR_SCHEMA(_base(length=22, byte_offset=12, byte_length=2, converter="div10"))
    assert cfg["byte_length"] == 2


def test_byte_length_requires_byte_offset():
    with pytest.raises(cv.Invalid, match="requires byte_offset"):
        SENSOR_SCHEMA(_base(length=4, byte_length=2))


def test_offset_plus_field_must_fit_block():
    with pytest.raises(cv.Invalid, match="must be <= length"):
        SENSOR_SCHEMA(_base(length=13, byte_offset=12, byte_length=2))


def test_converter_checked_against_extracted_width_not_block():
    # div10 supports 1-2 byte fields; a 2-byte extract is fine even though the
    # block read is 22 bytes (which div10 would never accept as a plain length).
    SENSOR_SCHEMA(_base(length=22, byte_offset=12, byte_length=2, converter="div10"))
    # sec2hour only supports 4-byte fields -> a 2-byte extract must be rejected.
    with pytest.raises(cv.Invalid, match="cannot decode"):
        SENSOR_SCHEMA(_base(length=22, byte_offset=12, byte_length=2, converter="sec2hour"))


# --- generator: interior fields emit aligned reads --------------------------


def _event(**kw):
    ev = gc.Event.__new__(gc.Event)
    for f in gc.Event.__dataclass_fields__:
        setattr(ev, f, None)
    ev.values = []
    ev.address = 0x7660
    ev.conversion = "noconv"
    ev.byte_length = 1
    ev.block_length = 2
    ev.byte_position = 1
    ev.name = "Interior Field"
    for k, v in kw.items():
        setattr(ev, k, v)
    return ev


def test_single_byte_interior_field_emits_aligned_block_read():
    plat, lines = gc.emit_entity(_event(), "standard")
    body = "\n".join(lines)
    assert plat == "sensor"
    # aligned: address is the block BASE, with byte_offset -- not addr+offset.
    assert "address: 0x7660" in body
    assert "byte_offset: 1" in body
    assert "length: 2" in body
    assert "0x7661" not in body  # never the interior address


def test_multi_byte_interior_field_emits_byte_length():
    ev = _event(byte_length=2, block_length=22, byte_position=12, conversion="Div10")
    plat, lines = gc.emit_entity(ev, "standard")
    body = "\n".join(lines)
    assert plat == "sensor"
    assert "address: 0x7660" in body  # block base
    assert "byte_offset: 12" in body
    assert "byte_length: 2" in body
    assert "length: 22" in body


def test_field_in_block_over_cap_stays_interior_or_comment():
    # A block bigger than the P300 cap can't be an aligned extract; the field
    # must not silently claim to be aligned.
    ev = _event(byte_length=1, block_length=MAX_P300_READ_LENGTH + 5, byte_position=40)
    plat, lines = gc.emit_entity(ev, "standard")
    body = "\n".join(lines)
    assert "byte_offset:" not in body


# --- enum option label de-duplication ---------------------------------------


def test_dedup_leaves_unique_labels_untouched():
    opts = [(0, "Off"), (1, "On"), (2, "Auto")]
    assert gc._dedup_option_labels(opts) == opts


def test_dedup_disambiguates_repeated_labels_with_raw_value():
    # The KF1_KonfiTemperaturprogramm case: values 6..8 all "Default".
    opts = [(0, "Passiv"), (6, "Default"), (7, "Default"), (8, "Default")]
    out = gc._dedup_option_labels(opts)
    labels = [lbl for _, lbl in out]
    assert len(labels) == len(set(labels))  # all unique now
    assert out[1] == (6, "Default")  # first occurrence kept clean
    assert out[2] == (7, "Default (0x07)")
    assert out[3] == (8, "Default (0x08)")


def test_dedup_preserves_values_and_order():
    opts = [(3, "X"), (1, "X"), (2, "Y")]
    out = gc._dedup_option_labels(opts)
    assert [v for v, _ in out] == [3, 1, 2]  # order/values intact


# --- schema: select/switch state-read extraction ------------------------------
# byte_offset/byte_length on the writable platforms mirror the sensor: with
# byte_offset, `length` is the block read at state_address and the enum /
# boolean field is byte_length (default 1) bytes at byte_offset. state_address
# is REQUIRED with byte_offset -- the write stays at `address`, the field's
# own register; writing field-width bytes at the block base would hit the
# wrong register.


def _select(**extra):
    cfg = {
        "name": _name(),
        "address": 0x7661,
        "options": {0x00: "Aus", 0x02: "Heizen"},
    }
    cfg.update(extra)
    return cfg


def _switch(**extra):
    cfg = {"name": _name(), "address": 0x7662}
    cfg.update(extra)
    return cfg


def test_select_accepts_block_extraction_with_state_address():
    cfg = SELECT_SCHEMA(_select(state_address=0x7660, length=2, byte_offset=1))
    assert cfg["length"] == 2
    assert cfg["byte_offset"] == 1


def test_select_byte_offset_requires_state_address():
    with pytest.raises(cv.Invalid, match="requires state_address"):
        SELECT_SCHEMA(_select(length=2, byte_offset=1))


def test_select_byte_length_requires_byte_offset():
    with pytest.raises(cv.Invalid, match="byte_length requires byte_offset"):
        SELECT_SCHEMA(_select(length=2, byte_length=2))


def test_select_options_checked_against_field_width_not_block():
    # A 2-byte option value in a 1-byte extracted field must be rejected even
    # though the block read is 6 bytes wide.
    with pytest.raises(cv.Invalid, match="does not fit 1 unsigned byte"):
        SELECT_SCHEMA(
            _select(
                state_address=0x7660,
                length=6,
                byte_offset=1,
                options={0x0100: "Zwei Byte"},
            )
        )
    # ...and a 2-byte field accepts it.
    SELECT_SCHEMA(
        _select(
            state_address=0x7660,
            length=6,
            byte_offset=4,
            byte_length=2,
            options={0x0100: "Zwei Byte"},
        )
    )


def test_select_field_must_fit_the_block():
    with pytest.raises(cv.Invalid, match="must be <= length"):
        SELECT_SCHEMA(_select(state_address=0x7660, length=2, byte_offset=1, byte_length=2))


def test_select_block_read_capped_at_p300_telegram():
    with pytest.raises(cv.Invalid, match="block read"):
        SELECT_SCHEMA(_select(state_address=0x7660, length=MAX_P300_READ_LENGTH + 1, byte_offset=0))


def test_select_plain_length_still_capped_at_two():
    with pytest.raises(cv.Invalid, match="length must be 1 or 2"):
        SELECT_SCHEMA(_select(length=3))


def test_switch_accepts_block_extraction_with_state_address():
    cfg = SWITCH_SCHEMA(_switch(state_address=0x7660, length=4, byte_offset=2))
    assert cfg["length"] == 4
    assert cfg["byte_offset"] == 2


def test_switch_byte_offset_requires_state_address():
    with pytest.raises(cv.Invalid, match="requires state_address"):
        SWITCH_SCHEMA(_switch(length=4, byte_offset=2))


def test_switch_values_checked_against_field_width_not_block():
    with pytest.raises(cv.Invalid, match="does not fit 1 unsigned byte"):
        SWITCH_SCHEMA(_switch(state_address=0x7660, length=4, byte_offset=2, on_value=0x0100))
    SWITCH_SCHEMA(_switch(state_address=0x7660, length=6, byte_offset=2, byte_length=2, on_value=0x0100))


# --- schema: number state-read extraction + read/write split -----------------
# Same semantics as select/switch: byte_offset requires state_address; with it,
# `length` is the block read and the numeric field is byte_length (default 1,
# max 4) bytes at byte_offset. The converter and the min/max encode checks run
# against the FIELD width, and codegen gives the write datapoint that width.


def _number(**extra):
    cfg = {
        "name": _name(),
        "address": 0x7952,
        "min_value": 0,
        "max_value": 100,
        "step": 1,
    }
    cfg.update(extra)
    return cfg


def test_number_accepts_block_extraction_with_state_address():
    cfg = NUMBER_SCHEMA(_number(state_address=0x7951, length=2, byte_offset=1))
    assert cfg["length"] == 2
    assert cfg["byte_offset"] == 1


def test_number_byte_offset_requires_state_address():
    with pytest.raises(cv.Invalid, match="requires state_address"):
        NUMBER_SCHEMA(_number(length=2, byte_offset=1))


def test_number_byte_length_requires_byte_offset():
    with pytest.raises(cv.Invalid, match="byte_length requires byte_offset"):
        NUMBER_SCHEMA(_number(length=2, byte_length=2))


def test_number_converter_checked_against_field_width_not_block():
    # div10 decodes 1-2 byte fields; a 2-byte extract inside a 22-byte block
    # is fine, the 22 must not hit the converter check.
    NUMBER_SCHEMA(
        _number(
            state_address=0x2500,
            length=22,
            byte_offset=12,
            byte_length=2,
            converter="div10",
            min_value=0,
            max_value=30,
            step=0.1,
        )
    )
    # ...but a field width the converter cannot encode is rejected.
    with pytest.raises(cv.Invalid, match="cannot encode/decode"):
        NUMBER_SCHEMA(
            _number(
                state_address=0x2500,
                length=22,
                byte_offset=12,
                byte_length=3,
                converter="div10",
            )
        )


def test_number_bounds_checked_against_field_width_not_block():
    # max 300 needs 2 raw bytes; a 1-byte extracted field must reject it even
    # though the block read is 4 bytes wide.
    with pytest.raises(cv.Invalid, match="does not fit 1 unsigned"):
        NUMBER_SCHEMA(_number(state_address=0x7951, length=4, byte_offset=1, max_value=300))


def test_number_field_must_fit_the_block():
    with pytest.raises(cv.Invalid, match="must be <= length"):
        NUMBER_SCHEMA(_number(state_address=0x7951, length=2, byte_offset=1, byte_length=2))


def test_number_block_read_capped_at_p300_telegram():
    with pytest.raises(cv.Invalid, match="block read"):
        NUMBER_SCHEMA(_number(state_address=0x7951, length=MAX_P300_READ_LENGTH + 1, byte_offset=0))


def test_number_plain_length_still_capped_at_four():
    with pytest.raises(cv.Invalid, match="between 1 and 4"):
        NUMBER_SCHEMA(_number(length=5))


def test_number_accepts_standalone_state_address_split():
    # The read/write split without extraction -- same as select/switch.
    cfg = NUMBER_SCHEMA(_number(address=0x2323, state_address=0x2501, length=1))
    assert cfg["state_address"] == 0x2501


# --- schema: read-only enum text_sensor extraction ----------------------------


def _enum_ts(**extra):
    cfg = {
        "type": "enum",
        "name": _name(),
        "address": 0x2500,
        "options": {0x00: "Aus", 0x02: "Heizen"},
    }
    cfg.update(extra)
    return cfg


def test_enum_text_sensor_accepts_block_extraction():
    cfg = TEXT_SENSOR_SCHEMA(_enum_ts(length=22, byte_offset=1))
    assert cfg["length"] == 22
    assert cfg["byte_offset"] == 1


def test_enum_text_sensor_field_must_fit_the_block():
    with pytest.raises(cv.Invalid, match="must be <= length"):
        TEXT_SENSOR_SCHEMA(_enum_ts(length=4, byte_offset=3, byte_length=2))


def test_enum_text_sensor_block_read_capped_at_p300_telegram():
    with pytest.raises(cv.Invalid, match="block read"):
        TEXT_SENSOR_SCHEMA(_enum_ts(length=MAX_P300_READ_LENGTH + 1, byte_offset=0))


def test_enum_text_sensor_plain_length_still_capped_at_four():
    with pytest.raises(cv.Invalid, match="between 1 and 4"):
        TEXT_SENSOR_SCHEMA(_enum_ts(length=22))


def test_enum_text_sensor_byte_length_requires_byte_offset():
    with pytest.raises(cv.Invalid, match="byte_length requires byte_offset"):
        TEXT_SENSOR_SCHEMA(_enum_ts(length=2, byte_length=2))
