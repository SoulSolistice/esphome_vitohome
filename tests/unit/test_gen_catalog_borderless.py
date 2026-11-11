"""A writable datapoint without usable borders is emitted read-only.

A ``number`` needs ``min_value`` / ``max_value`` / ``step``. Where the export
gives none that fits, the generator used to emit ``0`` / ``0`` / ``1``: valid
config, and a live entity whose only possible write is 0, to a parameter whose
real range nobody knows (1,972 of them over 172 catalogs, 2026-11-10 audit).
The datapoint now goes down the read-only path instead, with a NOTE.

Run:
    python -m pytest tests/unit/test_gen_catalog_borderless.py -q
"""

import os
import sys
from types import SimpleNamespace

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_SCRIPTS = os.path.join(_REPO_ROOT, "scripts")
for _p in (_SCRIPTS, _REPO_ROOT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import gen_catalog as gc  # noqa: E402


def _event(**overrides):
    """Stand-in for ``gen_catalog.Event``: a writable 1-byte raw value."""
    base = {
        "id": "1",
        "name": "Test Parameter",
        "address": 0x2500,
        "conversion": "",
        "access_type": 3,  # read+write
        "block_length": 0,
        "byte_length": 1,
        "byte_position": 0,
        "bit_length": 0,
        "bit_position": 0,
        "lower": "",
        "upper": "",
        "stepping": "",
        "enum_type": False,
        "unit": "",
        "tech": "test_parameter",
        "token": "Test~0x2500",
        "fc_read": "",
        "fc_write": "",
        "conv_factor": None,
        "conv_offset": None,
        "block_factor": None,
        "values": [],
    }
    base.update(overrides)
    return SimpleNamespace(**base)


def _emit(**overrides):
    platform, lines = gc.emit_entity(_event(**overrides), "full")
    return platform, "\n".join(lines)


def test_borders_make_a_number():
    platform, body = _emit(lower="0", upper="100", stepping="1")
    assert platform == "number"
    assert "min_value: 0" in body
    assert "max_value: 100" in body
    assert "step: 1" in body


def test_no_borders_is_a_read_only_sensor():
    platform, body = _emit()
    assert platform == "sensor"
    assert "min_value:" not in body
    assert "max_value:" not in body
    assert "CAUTION: Vitosoft marks this datapoint WRITABLE" in body


def test_zero_step_is_read_only():
    platform, body = _emit(lower="0", upper="100", stepping="0")
    assert platform == "sensor"
    assert "min_value:" not in body


def test_a_border_outside_the_byte_width_is_read_only():
    # 2000 does not fit one unsigned byte.
    platform, body = _emit(lower="0", upper="2000", stepping="1")
    assert platform == "sensor"
    assert "min_value:" not in body
    assert "CAUTION: Vitosoft marks this datapoint WRITABLE" in body


def test_no_placeholder_range_is_ever_emitted():
    for overrides in (
        {},
        {"byte_length": 2},
        {"stepping": "0", "lower": "0", "upper": "10"},
        {"block_length": 2, "byte_length": 1, "byte_position": 1},
    ):
        _platform, body = _emit(**overrides)
        assert "max_value: 0\n" not in body + "\n"


def test_interior_field_without_borders_reads_the_block():
    # Read-only extraction: the block is read at its base, never an interior
    # address.
    platform, body = _emit(address=0x7951, block_length=2, byte_length=1, byte_position=1)
    assert platform == "sensor"
    assert "address: 0x7951" in body
    assert "0x7952" not in body
    assert "byte_offset: 1" in body


def test_the_catalog_header_counts_them():
    # The mini export has exactly one writable datapoint without borders
    # (event 7); the header names the count and each entry carries the CAUTION.
    catalog = gc.load_catalog(os.path.join(os.path.dirname(__file__), "fixtures"))
    out = gc.generate(catalog, "VTestHO1_99", "full", None, None)
    assert "# WRITABLE WITHOUT A RANGE: 1 datapoint(s)" in out
    assert out.count(gc.NO_BORDERS_CAUTION[0]) == 1
    assert "max_value: 0\n" not in out


def test_an_empty_range_is_read_only():
    # Borders 0..0 with step 1 are in the export for 94 datapoints. A range
    # that holds one value is no range to write in.
    platform, body = _emit(lower="0", upper="0", stepping="1")
    assert platform == "sensor"
    assert "min_value:" not in body
    assert "CAUTION: Vitosoft marks this datapoint WRITABLE" in body
