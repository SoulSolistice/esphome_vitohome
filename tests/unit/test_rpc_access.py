"""`access: rpc` with an optional `prefix:` -- a P300 remote procedure call read.

A remote procedure (function code 0x07) is read with the parameter the catalog
gives it (PrefixRead), or with none. `prefix:` belongs to `access: rpc` and
means nothing without it; `access: rpc` is offered only on read-only platforms,
and only under P300, the one protocol with such a telegram.
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
    _RPC_DOMAINS,
    _WRITE_DOMAINS,
    ACCESS_RPC,
    CONF_ACCESS,
    CONF_PREFIX,
    CONF_PROTOCOL,
    CONF_VITOHOME_ID,
    RPC_PREFIX_MAX,
    RPC_PREFIX_SCHEMA,
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
    out = {"platform": "vitohome", CONF_NAME: "dp", CONF_VITOHOME_ID: _HUB_ID, CONF_ADDRESS: 0xA010}
    if extra:
        out.update(extra)
    return out


def _rpc(prefix=(0x00,)) -> dict:
    return _entity({CONF_ACCESS: read_access("rpc"), CONF_PREFIX: list(prefix)})


@pytest.mark.parametrize("domain", _RPC_DOMAINS)
def test_rpc_with_prefix_is_accepted_under_p300(domain):
    _validate("P300", {domain: [_rpc()]})
    _validate("VS2", {domain: [_rpc((0x00, 0x01, 0x02))]})


@pytest.mark.parametrize("protocol", ["KW", "VS1", "GWG"])
@pytest.mark.parametrize("domain", _RPC_DOMAINS)
def test_rpc_is_refused_where_there_is_no_such_telegram(protocol, domain):
    with pytest.raises(cv.Invalid, match="no remote procedure call telegram"):
        _validate(protocol, {domain: [_rpc()]})


@pytest.mark.parametrize("domain", _RPC_DOMAINS)
def test_rpc_without_prefix_is_a_call_without_a_parameter(domain):
    # The boiler-sequence values of a cascade controller are read this way.
    _validate("P300", {domain: [_entity({CONF_ACCESS: read_access("rpc")})]})


@pytest.mark.parametrize("domain", _RPC_DOMAINS)
def test_a_prefix_without_rpc_is_refused(domain):
    with pytest.raises(cv.Invalid, match="means nothing without 'access: rpc'"):
        _validate("P300", {domain: [_entity({CONF_PREFIX: [0x00]})]})


def test_rpc_is_not_a_gwg_mode_under_p300():
    # The non-GWG branch refuses every GWG mode; rpc must pass it.
    _validate("P300", {"sensor": [_rpc()], "binary_sensor": [_rpc((0x01,))]})
    with pytest.raises(cv.Invalid, match="selects a GWG TYPE byte"):
        _validate("P300", {"sensor": [_entity({CONF_ACCESS: read_access("eeprom")})]})


def test_rpc_is_only_offered_on_platforms_that_never_write():
    assert not set(_RPC_DOMAINS) & set(_WRITE_DOMAINS)


def test_read_access_takes_rpc_and_every_gwg_mode():
    assert read_access("RPC") == ACCESS_RPC
    assert str(read_access("eeprom")) == "eeprom"
    with pytest.raises(cv.Invalid, match="or 'rpc'"):
        read_access("bogus")


def test_prefix_is_one_to_rpc_prefix_max_bytes():
    assert RPC_PREFIX_SCHEMA(0x02) == [0x02]
    assert RPC_PREFIX_SCHEMA([0x00, 0x0A]) == [0x00, 0x0A]
    with pytest.raises(cv.Invalid):
        RPC_PREFIX_SCHEMA([])
    with pytest.raises(cv.Invalid):
        RPC_PREFIX_SCHEMA(list(range(RPC_PREFIX_MAX + 1)))
    with pytest.raises(cv.Invalid):
        RPC_PREFIX_SCHEMA([0x100])
