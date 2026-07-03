"""Unit tests for the switch platform: config validation (switch.py) and the
catalog generator's boolean-pair heuristic (gen_catalog._boolean_pair).

The heuristic tests encode the design rule that two options alone do NOT make
a boolean -- Celsius/Fahrenheit or Einkessel/Mehrkessel are choices, and a
switch card showing "on" would be meaningless for them.
"""

import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
for _p in (_REPO_ROOT, os.path.join(_REPO_ROOT, "scripts")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import esphome.config_validation as cv  # noqa: E402
from esphome.core import CORE  # noqa: E402
from gen_catalog import _boolean_pair  # noqa: E402

from components.vitohome.switch import CONFIG_SCHEMA  # noqa: E402


@pytest.fixture(autouse=True)
def _fresh_core():
    # ESPHome registers validated entity names in CORE to enforce per-platform
    # uniqueness; without a reset, the second CONFIG_SCHEMA call in this module
    # fails with "Duplicate switch entity ... found".
    CORE.reset()
    yield
    CORE.reset()


_NAME_SEQ = iter(range(1000))


def _cfg(**overrides):
    base = {"name": f"Partybetrieb HK1 {next(_NAME_SEQ)}", "address": 0x2330}
    base.update(overrides)
    return base


# --- switch.py validation ---------------------------------------------------


def test_minimal_switch_validates_with_defaults():
    cfg = CONFIG_SCHEMA(_cfg())
    assert cfg["on_value"] == 1
    assert cfg["off_value"] == 0
    assert cfg["length"] == 1
    assert cfg["read_back"] is True
    assert str(cfg["restore_mode"]) == "DISABLED"


def test_state_address_split_accepted():
    cfg = CONFIG_SCHEMA(_cfg(state_address=0x2303))
    assert cfg["state_address"] == 0x2303


def test_non_default_values_accepted():
    # K8A_KonfiAnzeigebedingungenAktiv: 175 = aktiv, 176 = inaktiv.
    cfg = CONFIG_SCHEMA(_cfg(on_value=175, off_value=176))
    assert (cfg["on_value"], cfg["off_value"]) == (175, 176)


def test_on_equals_off_rejected():
    with pytest.raises(cv.Invalid, match="must differ"):
        CONFIG_SCHEMA(_cfg(on_value=1, off_value=1))


def test_off_value_in_on_values_rejected():
    with pytest.raises(cv.Invalid, match="cannot also be in on_values"):
        CONFIG_SCHEMA(_cfg(on_values=[1, 0]))


def test_value_must_fit_length():
    with pytest.raises(cv.Invalid, match="does not fit 1 unsigned byte"):
        CONFIG_SCHEMA(_cfg(on_value=0x1FF))


def test_boot_restore_modes_rejected():
    # Any restore mode other than DISABLED would WRITE to the heater at boot.
    with pytest.raises(cv.Invalid, match="restore_mode must stay DISABLED"):
        CONFIG_SCHEMA(_cfg(restore_mode="RESTORE_DEFAULT_OFF"))


def test_inverted_blocked():
    with pytest.raises(cv.Invalid):
        CONFIG_SCHEMA(_cfg(inverted=True))


# --- gen_catalog._boolean_pair ------------------------------------------------


def test_ein_aus_pair_detected():
    assert _boolean_pair([(0, "AUS"), (1, "EIN")]) == (1, 0, "EIN", "AUS")


def test_coding_prefix_stripped_and_order_independent():
    # "0 inaktiv" / "1 aktiv" -- labels echo the coding value.
    assert _boolean_pair([(1, "1 aktiv"), (0, "0 inaktiv")]) == (1, 0, "1 aktiv", "0 inaktiv")


def test_non_zero_one_values_supported():
    # K8A: 175 = aktiv, 176 = inaktiv.
    assert _boolean_pair([(175, "aktiv"), (176, "inaktiv")]) == (175, 176, "aktiv", "inaktiv")


def test_negation_pair_detected():
    got = _boolean_pair([(0, "nicht vorhanden"), (1, "vorhanden")])
    assert got == (1, 0, "vorhanden", "nicht vorhanden")


def test_ein_aus_token_stem_detected():
    # Unresolved Neptun pump labels: the enum type's own name says on/off.
    got = _boolean_pair([(0, "Allgemein_Zustand_Ein_Aus~0"), (1, "Allgemein_Zustand_Ein_Aus~1")])
    assert got == (1, 0, "Ein", "Aus")


def test_choice_pairs_stay_selects():
    assert _boolean_pair([(0, "0 Celsius"), (1, "1 Fahrenheit")]) is None
    assert _boolean_pair([(1, "1 Einkessel"), (2, "2 Mehrkessel VI-Kaskade")]) is None
    assert _boolean_pair([(0, "0 Grundzustand"), (1, "1 Wartung")]) is None
    assert _boolean_pair([(0, "0 Abschalt-Reduziert"), (1, "1 Heizbetrieb")]) is None


def test_three_options_never_boolean():
    assert _boolean_pair([(0, "AUS"), (1, "EIN"), (2, "AUTO")]) is None
