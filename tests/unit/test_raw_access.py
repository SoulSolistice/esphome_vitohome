"""`raw_access` / `unsafe_raw_writes` -- the gates of queue_raw_op().

queue_raw_op() sends any telegram of any layer (raw_op.h). `raw_access:
unrestricted` compiles its sending branch and needs the raw lane it queues on;
`unsafe_raw_writes: true` adds the telegrams that may change the controller and
is meaningless without the first. Both default off.
"""

import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

import esphome.config_validation as cv  # noqa: E402

from components.vitohome import (  # noqa: E402
    CONF_RAW_ACCESS,
    CONF_RAW_QUEUE_SIZE,
    CONF_UNSAFE_RAW_WRITES,
    CONFIG_SCHEMA,
    RAW_ACCESS_STANDARD,
    RAW_ACCESS_UNRESTRICTED,
    _validate_raw_access,
)


def _hub(**options) -> dict:
    out = {CONF_RAW_ACCESS: RAW_ACCESS_STANDARD, CONF_UNSAFE_RAW_WRITES: False, CONF_RAW_QUEUE_SIZE: 0}
    out.update(options)
    return out


def test_the_default_refuses_and_needs_nothing():
    assert _validate_raw_access(_hub()) == _hub()


def test_unrestricted_with_a_lane_is_accepted():
    _validate_raw_access(_hub(**{CONF_RAW_ACCESS: RAW_ACCESS_UNRESTRICTED, CONF_RAW_QUEUE_SIZE: 1}))


def test_unrestricted_needs_the_raw_lane():
    with pytest.raises(cv.Invalid, match=CONF_RAW_QUEUE_SIZE) as info:
        _validate_raw_access(_hub(**{CONF_RAW_ACCESS: RAW_ACCESS_UNRESTRICTED}))
    assert info.value.path == [CONF_RAW_ACCESS]


def test_unsafe_writes_need_unrestricted():
    with pytest.raises(cv.Invalid, match="unrestricted") as info:
        _validate_raw_access(_hub(**{CONF_UNSAFE_RAW_WRITES: True, CONF_RAW_QUEUE_SIZE: 4}))
    assert info.value.path == [CONF_UNSAFE_RAW_WRITES]


def test_unsafe_writes_on_top_of_unrestricted_are_accepted():
    _validate_raw_access(
        _hub(**{CONF_RAW_ACCESS: RAW_ACCESS_UNRESTRICTED, CONF_UNSAFE_RAW_WRITES: True, CONF_RAW_QUEUE_SIZE: 1})
    )


def test_the_hub_schema_runs_the_gate_check():
    # cv.All(schema, ..., _validate_raw_access): the check is wired in, not
    # just defined.
    validators = getattr(CONFIG_SCHEMA, "validators", ())
    assert _validate_raw_access in validators


@pytest.mark.parametrize("value", ["standard", "unrestricted", "UNRESTRICTED"])
def test_raw_access_takes_its_two_values(value):
    assert cv.one_of(RAW_ACCESS_STANDARD, RAW_ACCESS_UNRESTRICTED, lower=True)(value) == value.lower()
