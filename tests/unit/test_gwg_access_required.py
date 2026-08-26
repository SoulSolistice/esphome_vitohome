"""Regression tests for the GWG access-mode *requirement* (2026-08-26).

Before this, an omitted `access:` silently meant `physical` everywhere. Two
asymmetric consequences, handled differently:

**Writes -- hard error.** Across all 22 Vitosoft GWG device tokens (4143
events) the writable datapoints are EEPROM_WRITE (1082) and BE_WRITE (280) and
*zero* PHYSICAL_WRITE (counted in scripts/gen_catalog.py's
FCWRITE_TO_GWG_ACCESS comment). So the implicit mode emits 0xC8, a write
telegram no documented GWG datapoint uses, at an address whose meaning is
mode-dependent -- writing to a datapoint other than the intended one, with no
error. Since GWG addresses are per-mode, there is nothing to fall back to.

This breaks no *generated* catalog: `_is_writable()` only calls a GWG datapoint
writable when its write mode equals its read mode, and a blank FCRead resolves
to `""`, which equals no write mode -- so a generated writable GWG entity
always carries an explicit `access:`. Only hand-written configs are affected.

**Reads -- warning only.** Vitosoft emits rows with a blank FCRead, and
gen_catalog.py deliberately maps blank to physical ("no access row" -> engine
default), so a missing mode on a read is not necessarily a mistake. Erroring
would force `access: physical` onto every datapoint about which Vitosoft itself
says nothing. It is still a silent assumption about datapoint identity, so it
warns.
"""

import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

import esphome.config_validation as cv  # noqa: E402
from esphome.const import CONF_ADDRESS, CONF_ID, CONF_NAME  # noqa: E402
import esphome.final_validate as fv  # noqa: E402

from components.vitohome import (  # noqa: E402
    _ACCESS_DOMAINS,
    _WRITE_DOMAINS,
    CONF_ACCESS,
    CONF_PROTOCOL,
    CONF_VITOHOME_ID,
    _final_validate,
)

_HUB_ID = "vito_test_hub"

# The access-capable platforms that only ever read. Derived the same way the
# validator does, so adding a platform to either tuple keeps these tests honest.
_READ_ONLY_DOMAINS = tuple(d for d in _ACCESS_DOMAINS if d not in _WRITE_DOMAINS)
# Writable platforms whose datapoint is a single flat address (climate is the
# exception -- two channels -- and gets its own tests).
_FLAT_WRITE_DOMAINS = tuple(d for d in _WRITE_DOMAINS if d != "climate")


def _validate(protocol: str, full_config: dict):
    token = fv.full_config.set(full_config)
    try:
        return _final_validate({CONF_PROTOCOL: protocol, CONF_ID: _HUB_ID})
    finally:
        fv.full_config.reset(token)


def _entity(extra: dict = None, name: str = "dp") -> dict:
    out = {"platform": "vitohome", CONF_NAME: name, CONF_VITOHOME_ID: _HUB_ID, CONF_ADDRESS: 0x01}
    if extra:
        out.update(extra)
    return out


def _climate(extra: dict = None, operating_mode: dict = None) -> dict:
    out = {
        "platform": "vitohome",
        CONF_NAME: "hk1",
        CONF_VITOHOME_ID: _HUB_ID,
        "target_address": 0x06,
    }
    if extra:
        out.update(extra)
    if operating_mode is not None:
        out["operating_mode"] = {CONF_ADDRESS: 0x23, **operating_mode}
    return out


# ---------------------------------------------------------------------------
# Writes: access is mandatory
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("domain", _FLAT_WRITE_DOMAINS)
def test_writable_entity_requires_access(domain):
    with pytest.raises(cv.Invalid, match="writes but does not set"):
        _validate("GWG", {domain: [_entity()]})


@pytest.mark.parametrize("domain", _FLAT_WRITE_DOMAINS)
def test_writable_entity_with_access_is_accepted(domain):
    _validate("GWG", {domain: [_entity({CONF_ACCESS: "eeprom"})]})


def test_rejection_explains_why_physical_cannot_be_defaulted():
    """The message has to carry the reason, or it reads as bureaucracy."""
    with pytest.raises(cv.Invalid, match="PHYSICAL_WRITE"):
        _validate("GWG", {"number": [_entity()]})


def test_writable_climate_requires_setpoint_slot():
    with pytest.raises(cv.Invalid, match="writes but does not set 'access'"):
        _validate("GWG", {"climate": [_climate()]})


def test_writable_climate_requires_operating_mode_slot():
    """The nested channel is a separate register and needs its own mode."""
    with pytest.raises(cv.Invalid, match=r"operating_mode\.access"):
        _validate("GWG", {"climate": [_climate({CONF_ACCESS: "eeprom"}, operating_mode={})]})


def test_writable_climate_with_both_slots_is_accepted():
    _validate(
        "GWG",
        {"climate": [_climate({CONF_ACCESS: "eeprom"}, operating_mode={CONF_ACCESS: "be"})]},
    )


def test_climate_without_operating_mode_needs_only_the_setpoint_slot():
    """The Betriebsart slot exists only when the optional block does -- the
    check must not demand a key that has nowhere to live."""
    _validate("GWG", {"climate": [_climate({CONF_ACCESS: "eeprom"})]})


# ---------------------------------------------------------------------------
# Reads: warning, not error
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("domain", _READ_ONLY_DOMAINS)
def test_read_only_entity_without_access_is_accepted(domain):
    """Must NOT raise -- blank FCRead legitimately means physical."""
    _validate("GWG", {domain: [_entity()]})


@pytest.mark.parametrize("domain", _READ_ONLY_DOMAINS)
def test_read_only_entity_without_access_warns(domain, caplog):
    _validate("GWG", {domain: [_entity()]})
    assert "assuming physical" in caplog.text
    assert "dp" in caplog.text


def test_read_warning_names_the_ambiguity(caplog):
    """A bare 'set access:' nag is ignorable; the reason is the useful part."""
    _validate("GWG", {"sensor": [_entity()]})
    assert "0x01 is five datapoints across four modes" in caplog.text


def test_read_only_entity_with_access_does_not_warn(caplog):
    _validate("GWG", {"sensor": [_entity({CONF_ACCESS: "virtual"})]})
    assert "assuming physical" not in caplog.text


@pytest.mark.parametrize("protocol", ["P300", "KW"])
def test_no_warning_off_gwg(protocol, caplog):
    """access has no meaning off GWG, so its absence is not noteworthy."""
    _validate(protocol, {"sensor": [_entity()]})
    assert "assuming physical" not in caplog.text


def test_warning_is_per_entity(caplog):
    _validate("GWG", {"sensor": [_entity(name="alpha"), _entity(name="beta")]})
    assert "alpha" in caplog.text
    assert "beta" in caplog.text


# ---------------------------------------------------------------------------
# Scope guards
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("protocol", ["P300", "KW"])
@pytest.mark.parametrize("domain", _FLAT_WRITE_DOMAINS)
def test_writable_entity_needs_no_access_off_gwg(protocol, domain):
    """The requirement is GWG-specific: elsewhere access is meaningless and
    setting it is itself an error, so demanding it would be unsatisfiable."""
    _validate(protocol, {domain: [_entity()]})


def test_entity_on_another_hub_is_untouched():
    other = _entity()
    other[CONF_VITOHOME_ID] = "some_other_hub"
    _validate("GWG", {"number": [other]})
