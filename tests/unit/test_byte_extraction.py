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
import gen_catalog as gc  # noqa: E402
from esphome.core import CORE  # noqa: E402

from components.vitohome import MAX_P300_READ_LENGTH  # noqa: E402
from components.vitohome.sensor import CONFIG_SCHEMA as SENSOR_SCHEMA  # noqa: E402


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
