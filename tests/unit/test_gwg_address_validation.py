"""Regression tests for the GWG 8-bit-address final validation.

GWG addresses a single byte: ``PacketGWG::createPacket()`` rejects any address
above 0xFF, and a rejected request never leaves the hub's dispatch lane -- one
such entity at the front of the read or write queue stalls that lane (and
everything queued behind it) permanently. ``_final_validate`` therefore hard
fails at ``esphome config`` time on any 16-bit address under ``protocol: GWG``.

These tests lock two things:

* the check covers EVERY platform that carries an Optolink address -- the flat
  ``address``/``state_address`` keys, climate's ``target_address``, and
  climate's NESTED ``operating_mode`` block. ``event:`` and ``climate:`` were
  missing from the original domain list, so a GWG config with a 16-bit fault
  slot or Betriebsart address passed ``esphome config`` and stalled at runtime;
* the check stays scoped: P300/KW configs, sub-0xFF GWG addresses, and entities
  bound to a different hub are untouched.

``_final_validate`` reads the whole-config snapshot from ESPHome's
``final_validate.full_config`` ContextVar; the tests set it the same way the
framework does.
"""

import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

import esphome.config_validation as cv  # noqa: E402
from esphome.const import CONF_ID, CONF_NAME  # noqa: E402
import esphome.final_validate as fv  # noqa: E402

from components.vitohome import CONF_PROTOCOL, CONF_VITOCONNECT_ID, _final_validate  # noqa: E402

_HUB_ID = "vito_test_hub"


def _validate(protocol: str, full_config: dict):
    """Run the hub's _final_validate against a synthetic whole-config snapshot,
    exactly as ESPHome would: the snapshot goes into the full_config
    ContextVar, the hub's own config into the validator. ESPHome calls final
    validators for their side effects only (esphome/config.py types them
    Callable[[ConfigType], None] and discards the return), so acceptance is
    simply "does not raise"."""
    token = fv.full_config.set(full_config)
    try:
        return _final_validate({CONF_PROTOCOL: protocol, CONF_ID: _HUB_ID})
    finally:
        fv.full_config.reset(token)


def _entity(domain_extra: dict, name: str = "dp") -> dict:
    entity = {"platform": "vitohome", CONF_NAME: name, CONF_VITOCONNECT_ID: _HUB_ID}
    entity.update(domain_extra)
    return entity


# --- the original flat-address domains (sanity: the check still fires) -------


def test_gwg_rejects_16bit_sensor_address():
    full = {"sensor": [_entity({"address": 0x0800})]}
    with pytest.raises(cv.Invalid, match="0x0800"):
        _validate("GWG", full)


def test_gwg_rejects_16bit_state_address():
    full = {"select": [_entity({"address": 0x30, "state_address": 0x2303})]}
    with pytest.raises(cv.Invalid, match="state_address 0x2303"):
        _validate("GWG", full)


# --- event: was missing from the domain list ---------------------------------


def test_gwg_rejects_16bit_event_address():
    full = {"event": [_entity({"address": 0x7507})]}
    with pytest.raises(cv.Invalid, match="event .*0x7507"):
        _validate("GWG", full)


# --- climate: flat target_address and the nested operating_mode block --------


def test_gwg_rejects_16bit_climate_target_address():
    full = {"climate": [_entity({"target_address": 0x2306})]}
    with pytest.raises(cv.Invalid, match="target_address 0x2306"):
        _validate("GWG", full)


def test_gwg_rejects_16bit_climate_operating_mode_address():
    full = {
        "climate": [
            _entity(
                {
                    "target_address": 0x23,
                    "operating_mode": {"address": 0x2323, "presets": []},
                }
            )
        ]
    }
    with pytest.raises(cv.Invalid, match="operating_mode.address 0x2323"):
        _validate("GWG", full)


def test_gwg_rejects_16bit_climate_operating_mode_state_address():
    full = {
        "climate": [
            _entity(
                {
                    "target_address": 0x23,
                    "operating_mode": {"address": 0x23, "state_address": 0x2301, "presets": []},
                }
            )
        ]
    }
    with pytest.raises(cv.Invalid, match="operating_mode.state_address 0x2301"):
        _validate("GWG", full)


# --- scoping: what must NOT trip the check ------------------------------------


def test_gwg_accepts_8bit_addresses_everywhere():
    full = {
        "sensor": [_entity({"address": 0x63})],
        "event": [_entity({"address": 0x22})],
        "climate": [
            _entity(
                {
                    "target_address": 0x23,
                    "operating_mode": {"address": 0x30, "state_address": 0x31, "presets": []},
                }
            )
        ],
    }
    _validate("GWG", full)  # acceptance == does not raise (return is discarded by ESPHome)


def test_p300_accepts_16bit_addresses():
    full = {
        "sensor": [_entity({"address": 0x0800})],
        "event": [_entity({"address": 0x7507})],
        "climate": [
            _entity(
                {
                    "target_address": 0x2306,
                    "operating_mode": {"address": 0x2323, "state_address": 0x2301, "presets": []},
                }
            )
        ],
    }
    _validate("P300", full)  # acceptance == does not raise (return is discarded by ESPHome)


def test_gwg_ignores_entities_bound_to_another_hub():
    other = _entity({"address": 0x7507})
    other[CONF_VITOCONNECT_ID] = "some_other_hub"
    full = {"event": [other]}
    _validate("GWG", full)  # acceptance == does not raise (return is discarded by ESPHome)


def test_gwg_ignores_non_vitohome_platforms():
    full = {"sensor": [{"platform": "template", CONF_NAME: "x", "address": 0x0800}]}
    _validate("GWG", full)  # acceptance == does not raise (return is discarded by ESPHome)
