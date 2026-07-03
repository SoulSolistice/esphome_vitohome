"""Unit tests for the force-refresh button platform (button.py)."""

import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

import esphome.config_validation as cv  # noqa: E402
from esphome.core import CORE  # noqa: E402

from components.vitohome.button import CONFIG_SCHEMA  # noqa: E402


@pytest.fixture(autouse=True)
def _fresh_core():
    # ESPHome registers validated entity names in CORE for uniqueness; see
    # tests/unit/test_switch.py for the background.
    CORE.reset()
    yield
    CORE.reset()


_NAME_SEQ = iter(range(1000))


def _name():
    return f"Refresh {next(_NAME_SEQ)}"


def test_minimal_button_validates_with_defaults():
    cfg = CONFIG_SCHEMA({"name": _name()})
    assert str(cfg["icon"]) == "mdi:refresh"
    assert str(cfg["entity_category"]) == "diagnostic"


def test_defaults_are_overridable():
    cfg = CONFIG_SCHEMA({"name": _name(), "icon": "mdi:reload", "entity_category": "config"})
    assert str(cfg["icon"]) == "mdi:reload"
    assert str(cfg["entity_category"]) == "config"


def test_no_address_key_exists():
    # The refresh button is hub-level: an address would be a config smell.
    with pytest.raises(cv.Invalid):
        CONFIG_SCHEMA({"name": _name(), "address": 0x2306})
