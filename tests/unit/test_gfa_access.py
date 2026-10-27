"""`access: gfa` -- a read in the burner control's address space (0x6B).

The burner control (GFA) is a second controller behind the Vitotronic; Vitosoft
marks its datapoints FCRead GFA_READ and defines no GFA write. `access: gfa` is
offered on read-only platforms only, and only under KW and P300, the protocols
that send the read (GWG has no such telegram).
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
    _GFA_DOMAINS,
    _WRITE_DOMAINS,
    ACCESS_GFA,
    CONF_ACCESS,
    CONF_PREFIX,
    CONF_PROTOCOL,
    CONF_VITOHOME_ID,
    GWG_ACCESS_MODES,
    _final_validate,
    read_access,
)

_HUB_ID = "vito_test_hub"


def _validate(protocol: str, full_config: dict):
    token = fv.full_config.set(full_config)
    try:
        return _final_validate({CONF_PROTOCOL: protocol, CONF_ID: _HUB_ID})
    finally:
        fv.full_config.reset(token)


def _entity(extra: dict = None) -> dict:
    # P80 of the burner control: the chip byte (0x24 = NEC on the reference unit).
    out = {"platform": "vitohome", CONF_NAME: "dp", CONF_VITOHOME_ID: _HUB_ID, CONF_ADDRESS: 0x4050}
    if extra:
        out.update(extra)
    return out


def _gfa() -> dict:
    return _entity({CONF_ACCESS: read_access("gfa")})


@pytest.mark.parametrize("protocol", ["KW", "VS1", "P300", "VS2"])
@pytest.mark.parametrize("domain", _GFA_DOMAINS)
def test_gfa_is_accepted_where_the_read_exists(protocol, domain):
    _validate(protocol, {domain: [_gfa()]})


@pytest.mark.parametrize("domain", _GFA_DOMAINS)
def test_gfa_is_refused_under_gwg(domain):
    with pytest.raises(cv.Invalid, match="no GFA read telegram"):
        _validate("GWG", {domain: [_gfa()]})


@pytest.mark.parametrize("protocol", ["KW", "P300"])
def test_gfa_is_not_a_gwg_mode(protocol):
    # The non-GWG branch refuses every GWG mode; gfa must pass it, next to an
    # ordinary read of the same address in the Vitotronic's own space.
    _validate(protocol, {"sensor": [_gfa(), _entity()], "binary_sensor": [_gfa()], "text_sensor": [_gfa()]})


def test_gfa_is_only_offered_on_platforms_that_never_write():
    assert not set(_GFA_DOMAINS) & set(_WRITE_DOMAINS)


def test_read_access_takes_gfa():
    assert read_access("gfa") == ACCESS_GFA
    assert read_access(" GFA ") == ACCESS_GFA
    with pytest.raises(cv.Invalid, match="or 'gfa'"):
        read_access("bogus")


def test_a_prefix_belongs_to_rpc_not_gfa():
    with pytest.raises(cv.Invalid, match="means nothing without 'access: rpc'"):
        _validate("KW", {"sensor": [_entity({CONF_ACCESS: read_access("gfa"), CONF_PREFIX: [0x00]})]})


def test_writable_platforms_do_not_take_gfa():
    # number/select/switch/text/climate validate `access:` as a GWG mode only.
    assert ACCESS_GFA not in GWG_ACCESS_MODES
    with pytest.raises(cv.Invalid):
        cv.enum(GWG_ACCESS_MODES, lower=True)("gfa")


def test_sensor_schema_takes_gfa():
    from components.vitohome.sensor import CONFIG_SCHEMA as SENSOR_SCHEMA

    conf = SENSOR_SCHEMA({CONF_NAME: "P06 fan speed", CONF_ADDRESS: 0x4006, "length": 1, CONF_ACCESS: "gfa"})
    assert conf[CONF_ACCESS] == ACCESS_GFA
