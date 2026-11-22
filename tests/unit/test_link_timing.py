"""`link_timing` -- the KW link's pauses (optolink_types.h, TimingPoint).

`enq_to_send` holds the 01 back after the device's ENQ, `answer_to_next` a
chained telegram after an answer. Each is bounded; the C++ side clamps to the
same bounds (test_limits_agree.py). P300 and GWG have no timing points yet, so
the option is refused there.
"""

import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

import esphome.config_validation as cv  # noqa: E402

from components.vitohome import (  # noqa: E402
    ANSWER_TO_NEXT_MAX_MS,
    CONF_ANSWER_TO_NEXT,
    CONF_ENQ_TO_SEND,
    CONF_LINK_TIMING,
    CONF_PROTOCOL,
    ENQ_TO_SEND_MAX_MS,
    LINK_TIMING_SCHEMA,
    _validate_link_timing,
)


def test_both_points_take_milliseconds():
    out = LINK_TIMING_SCHEMA({CONF_ENQ_TO_SEND: "30ms", CONF_ANSWER_TO_NEXT: "0ms"})
    assert out[CONF_ENQ_TO_SEND].total_milliseconds == 30
    assert out[CONF_ANSWER_TO_NEXT].total_milliseconds == 0


@pytest.mark.parametrize(
    ("key", "bound"), [(CONF_ENQ_TO_SEND, ENQ_TO_SEND_MAX_MS), (CONF_ANSWER_TO_NEXT, ANSWER_TO_NEXT_MAX_MS)]
)
def test_each_point_stops_at_its_bound(key, bound):
    assert LINK_TIMING_SCHEMA({key: f"{bound}ms"})[key].total_milliseconds == bound
    with pytest.raises(cv.Invalid):
        LINK_TIMING_SCHEMA({key: f"{bound + 1}ms"})


def test_kw_takes_it():
    config = {CONF_PROTOCOL: "KW", CONF_LINK_TIMING: {}}
    assert _validate_link_timing(config) == config


@pytest.mark.parametrize("protocol", ["P300", "GWG"])
def test_the_other_protocols_refuse_it(protocol):
    with pytest.raises(cv.Invalid, match="KW") as info:
        _validate_link_timing({CONF_PROTOCOL: protocol, CONF_LINK_TIMING: {}})
    assert info.value.path == [CONF_LINK_TIMING]


@pytest.mark.parametrize("protocol", ["P300", "GWG", "KW"])
def test_without_it_every_protocol_passes(protocol):
    _validate_link_timing({CONF_PROTOCOL: protocol})
