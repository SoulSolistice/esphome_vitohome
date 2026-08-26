"""Regression tests for two climate-platform fixes (2026-08-26).

**GWG access modes on climate.** ``climate`` was in ``_ADDRESS_DOMAINS`` (so
its addresses were GWG-validated as single-byte, i.e. climate is genuinely
reachable under ``protocol: GWG``) but was missing from ``_ACCESS_DOMAINS`` --
so it was the one addressed platform that could not select a TYPE byte. That
matters specifically on GWG, where the access mode is part of the address:
Vitosoft's own GWG tables carry five different datapoints at address 0x01
across four modes, so a Betriebsart register living in EEPROM space would have
been read and written in PHYSICAL space -- a different datapoint, silently.

Climate is also the only platform with a NESTED datapoint (``operating_mode``),
so it carries two independent access modes, one per channel. Both channels
read AND write, which is why climate joins ``_WRITE_DOMAINS`` too: a read-only
KMBUS mode there would compile, accept values from Home Assistant, and never
reach the device.

**Preset ``mode:`` is now required.** It used to default to ``heat``, which
silently classified an unannotated Standby/Frostschutz row as
CLIMATE_MODE_HEAT. Combined with ``control()``'s documented "first preset with
that mode wins" rule, a config listing such a preset first would turn the
heating OFF in response to an ``hvac_mode: heat`` call from the HA thermostat
card or a voice assistant.
"""

import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

import esphome.config_validation as cv  # noqa: E402
from esphome.const import CONF_ADDRESS, CONF_ID, CONF_MODE, CONF_NAME  # noqa: E402
import esphome.final_validate as fv  # noqa: E402

from components.vitohome import (  # noqa: E402
    _ACCESS_DOMAINS,
    _WRITE_DOMAINS,
    CONF_ACCESS,
    CONF_PROTOCOL,
    CONF_VITOHOME_ID,
    _final_validate,
)
from components.vitohome.climate import (  # noqa: E402
    CONF_OPERATING_MODE,
    CONF_READ,
    CONF_TARGET_ADDRESS,
    CONF_WRITE,
    _validate_presets,
)

_HUB_ID = "vito_test_hub"


def _validate(protocol: str, full_config: dict):
    """Run the hub's _final_validate against a synthetic whole-config snapshot.
    Final validators are typed Callable[[ConfigType], None] and their return is
    discarded, so acceptance is "does not raise"."""
    token = fv.full_config.set(full_config)
    try:
        return _final_validate({CONF_PROTOCOL: protocol, CONF_ID: _HUB_ID})
    finally:
        fv.full_config.reset(token)


def _climate(extra: dict = None, operating_mode: dict = None) -> dict:
    entity = {
        "platform": "vitohome",
        CONF_NAME: "hk1",
        CONF_VITOHOME_ID: _HUB_ID,
        CONF_TARGET_ADDRESS: 0x06,  # single-byte: valid under GWG
    }
    if extra:
        entity.update(extra)
    if operating_mode is not None:
        entity[CONF_OPERATING_MODE] = {CONF_ADDRESS: 0x23, **operating_mode}
    return entity


# ---------------------------------------------------------------------------
# Domain membership
# ---------------------------------------------------------------------------


def test_climate_is_an_access_domain():
    """The gap this fixes: climate was GWG-addressable but not access-capable."""
    assert "climate" in _ACCESS_DOMAINS


def test_climate_is_a_write_domain():
    """Both climate channels write, so read-only modes must be rejected there."""
    assert "climate" in _WRITE_DOMAINS


# ---------------------------------------------------------------------------
# access: accepted on GWG, both channels
# ---------------------------------------------------------------------------


def test_setpoint_access_accepted_under_gwg():
    _validate("GWG", {"climate": [_climate({CONF_ACCESS: "eeprom"})]})


def test_operating_mode_access_accepted_under_gwg():
    # Both slots must be set: climate writes, and every writable slot now needs
    # an explicit mode (see test_writable_climate_requires_both_slots).
    _validate(
        "GWG",
        {"climate": [_climate({CONF_ACCESS: "eeprom"}, operating_mode={CONF_ACCESS: "be"})]},
    )


def test_both_channels_may_use_different_modes():
    """The two channels are separate registers; nothing forces a shared mode."""
    _validate(
        "GWG",
        {"climate": [_climate({CONF_ACCESS: "eeprom"}, operating_mode={CONF_ACCESS: "be"})]},
    )


# ---------------------------------------------------------------------------
# Read-only (KMBUS) modes rejected on a writing channel
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("mode", ["kmbus_ram", "kmbus_eeprom"])
def test_readonly_mode_rejected_on_setpoint(mode):
    with pytest.raises(cv.Invalid, match="read-only"):
        _validate("GWG", {"climate": [_climate({CONF_ACCESS: mode})]})


@pytest.mark.parametrize("mode", ["kmbus_ram", "kmbus_eeprom"])
def test_readonly_mode_rejected_on_operating_mode(mode):
    """The nested block must be reached too -- the original loop only looked at
    the entity's top level, so this one would have slipped through."""
    with pytest.raises(cv.Invalid, match="read-only"):
        _validate(
            "GWG",
            {"climate": [_climate({CONF_ACCESS: "eeprom"}, operating_mode={CONF_ACCESS: mode})]},
        )


def test_readonly_rejection_names_the_nested_key():
    """The message must point at operating_mode.access, not a bare 'access'."""
    with pytest.raises(cv.Invalid, match=r"operating_mode\.access"):
        _validate(
            "GWG",
            {"climate": [_climate({CONF_ACCESS: "eeprom"}, operating_mode={CONF_ACCESS: "kmbus_ram"})]},
        )


# ---------------------------------------------------------------------------
# access: rejected under non-GWG protocols
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("protocol", ["P300", "KW"])
def test_setpoint_access_rejected_off_gwg(protocol):
    with pytest.raises(cv.Invalid, match="has no effect under protocol"):
        _validate(protocol, {"climate": [_climate({CONF_ACCESS: "virtual"})]})


@pytest.mark.parametrize("protocol", ["P300", "KW"])
def test_operating_mode_access_rejected_off_gwg(protocol):
    with pytest.raises(cv.Invalid, match="has no effect under protocol"):
        _validate(protocol, {"climate": [_climate(operating_mode={CONF_ACCESS: "virtual"})]})


@pytest.mark.parametrize("protocol", ["P300", "KW"])
def test_climate_without_access_is_fine_off_gwg(protocol):
    """Scope guard: the unset path must stay untouched where access has no
    meaning. Only GWG requires it (see the writable-entity tests)."""
    _validate(protocol, {"climate": [_climate(operating_mode={})]})


# ---------------------------------------------------------------------------
# Preset mode: now required
# ---------------------------------------------------------------------------


def _preset(name: str, write: int, read: list, mode: str = None) -> dict:
    row = {CONF_NAME: name, CONF_WRITE: write, CONF_READ: read}
    if mode is not None:
        row[CONF_MODE] = mode
    return row


def test_preset_mode_is_required():
    """The core fix: an omitted mode: no longer silently becomes 'heat'."""
    with pytest.raises(cv.Invalid):
        _validate_presets([_preset("Normal", 0x04, [0x02])])


def test_preset_mode_explicit_is_accepted():
    out = _validate_presets([_preset("Normal", 0x04, [0x02], "heat")])
    assert str(out[0][CONF_MODE]) == "heat"


def test_standby_can_be_declared_off():
    """The case that motivated this: a Standby row is 'off', not 'heat'."""
    out = _validate_presets(
        [
            _preset("Standby", 0x00, [0x05], "off"),
            _preset("Normal", 0x04, [0x02], "heat"),
        ]
    )
    assert str(out[0][CONF_MODE]) == "off"
    assert str(out[1][CONF_MODE]) == "heat"


def test_shared_mode_warns_and_names_the_winner(caplog):
    """Several presets per mode stays LEGAL (Normal/Reduziert are both heat),
    but must warn, because control() silently resolves a bare mode tap to the
    first one in list order."""
    _validate_presets(
        [
            _preset("Normal", 0x04, [0x02], "heat"),
            _preset("Reduziert", 0x03, [0x01], "heat"),
        ]
    )
    assert "'Normal'" in caplog.text
    assert "'Reduziert'" in caplog.text


def test_unique_modes_do_not_warn(caplog):
    _validate_presets(
        [
            _preset("Normal", 0x04, [0x02], "heat"),
            _preset("Abschaltbetrieb", 0x00, [0x05], "off"),
        ]
    )
    assert "hvac mode" not in caplog.text
