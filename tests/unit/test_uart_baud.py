"""The Optolink uart's baud rate: nominal 4800, a band around it for
controllers whose clock runs off-nominal (#35)."""

import logging
import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

import esphome.config_validation as cv  # noqa: E402

from components.vitohome import _MAX_BAUD_RATE, _MIN_BAUD_RATE, _validate_baud_rate  # noqa: E402


def test_band_is_ten_percent_around_4800():
    assert (_MIN_BAUD_RATE, _MAX_BAUD_RATE) == (4320, 5280)


def test_nominal_rate_is_accepted_silently(caplog):
    with caplog.at_level(logging.WARNING):
        assert _validate_baud_rate(4800) == 4800
    assert caplog.text == ""


@pytest.mark.parametrize("rate", [4600, 5200, _MIN_BAUD_RATE, _MAX_BAUD_RATE])
def test_off_nominal_rate_in_band_is_accepted_with_a_warning(rate, caplog):
    with caplog.at_level(logging.WARNING):
        assert _validate_baud_rate(rate) == rate
    assert f"{rate} baud instead of 4800" in caplog.text
    assert "off-nominal" in caplog.text


@pytest.mark.parametrize("rate", [_MIN_BAUD_RATE - 1, _MAX_BAUD_RATE + 1, 2400, 9600, 115200])
def test_rate_outside_band_is_rejected(rate):
    with pytest.raises(cv.Invalid, match=f"requires 4800 baud .* got {rate}"):
        _validate_baud_rate(rate)


def test_rejection_names_the_accepted_band():
    with pytest.raises(cv.Invalid, match="4320-5280 is accepted"):
        _validate_baud_rate(9600)
