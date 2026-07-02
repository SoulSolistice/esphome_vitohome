"""Unit tests for the climate platform's preset validation.

``on_mode_read`` resolves a state byte to the FIRST preset whose read set
contains it, so a read value shared by two presets would silently shadow the
later one and misreport the mode. ``_validate_presets`` must reject that at
``esphome config`` time.
"""

import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

import esphome.config_validation as cv  # noqa: E402

from components.vitohome.climate import _validate_presets  # noqa: E402


def _preset(name, write, read, mode="heat"):
    return {"name": name, "write": write, "read": read, "mode": mode}


def test_valid_presets_pass():
    value = _validate_presets(
        [
            _preset("Abschalt", 0x00, [0x00], mode="off"),
            _preset("Nur WW", 0x01, [0x01]),
            _preset("Heizen + WW", 0x02, [0x02, 0x03]),
        ]
    )
    assert [p["name"] for p in value] == ["Abschalt", "Nur WW", "Heizen + WW"]


def test_empty_presets_rejected():
    with pytest.raises(cv.Invalid, match="at least one preset"):
        _validate_presets([])


def test_duplicate_names_rejected():
    with pytest.raises(cv.Invalid, match="unique"):
        _validate_presets([_preset("A", 0x00, [0x00]), _preset("A", 0x01, [0x01])])


def test_duplicate_read_value_across_presets_rejected():
    with pytest.raises(cv.Invalid, match="0x02.*exactly one preset"):
        _validate_presets(
            [
                _preset("Nur WW", 0x01, [0x01, 0x02]),
                _preset("Heizen + WW", 0x02, [0x02]),
            ]
        )


def test_duplicate_write_values_are_allowed():
    # Two presets may command the same byte (e.g. two labels for one command
    # space value with disjoint read sets); only READ duplicates break state
    # resolution.
    value = _validate_presets(
        [
            _preset("A", 0x01, [0x01]),
            _preset("B", 0x01, [0x02]),
        ]
    )
    assert len(value) == 2
