"""`link_timing` -- the link's pauses (optolink_types.h, TimingPoint).

KW: `enq_to_send` holds the 01 back after the device's ENQ, `answer_to_next`
a chained telegram after an answer. P300 has both (the first on the KW
detour, the second after our ACK) and `enq_to_sync` (the ENQ after our EOT ->
our 16 00 00) and `frame_to_ack` (an answer frame -> our ACK). Each is
bounded; the C++ side clamps to the same bounds (test_limits_agree.py). GWG
has `enq_to_send` and `ack_to_telegram` (its 01 -> the rest of the telegram).
"""

import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

import esphome.config_validation as cv  # noqa: E402

from components.vitohome import (  # noqa: E402
    ACK_TO_TELEGRAM_MAX_MS,
    ANSWER_TO_NEXT_MAX_MS,
    CONF_ACK_TO_TELEGRAM,
    CONF_ANSWER_TO_NEXT,
    CONF_ENQ_TO_SEND,
    CONF_ENQ_TO_SYNC,
    CONF_FRAME_TO_ACK,
    CONF_LINK_TIMING,
    CONF_PROTOCOL,
    ENQ_TO_SEND_MAX_MS,
    ENQ_TO_SYNC_MAX_MS,
    FRAME_TO_ACK_MAX_MS,
    LINK_TIMING_SCHEMA,
    _validate_link_timing,
)

BOUNDS = [
    (CONF_ENQ_TO_SEND, ENQ_TO_SEND_MAX_MS),
    (CONF_ANSWER_TO_NEXT, ANSWER_TO_NEXT_MAX_MS),
    (CONF_ENQ_TO_SYNC, ENQ_TO_SYNC_MAX_MS),
    (CONF_FRAME_TO_ACK, FRAME_TO_ACK_MAX_MS),
    (CONF_ACK_TO_TELEGRAM, ACK_TO_TELEGRAM_MAX_MS),
]
P300_POINTS = (CONF_ENQ_TO_SEND, CONF_ANSWER_TO_NEXT, CONF_ENQ_TO_SYNC, CONF_FRAME_TO_ACK)


def test_the_points_take_milliseconds():
    out = LINK_TIMING_SCHEMA({CONF_ENQ_TO_SEND: "30ms", CONF_ANSWER_TO_NEXT: "0ms", CONF_FRAME_TO_ACK: "50ms"})
    assert out[CONF_ENQ_TO_SEND].total_milliseconds == 30
    assert out[CONF_ANSWER_TO_NEXT].total_milliseconds == 0
    assert out[CONF_FRAME_TO_ACK].total_milliseconds == 50


@pytest.mark.parametrize(("key", "bound"), BOUNDS)
def test_each_point_stops_at_its_bound(key, bound):
    assert LINK_TIMING_SCHEMA({key: f"{bound}ms"})[key].total_milliseconds == bound
    with pytest.raises(cv.Invalid):
        LINK_TIMING_SCHEMA({key: f"{bound + 1}ms"})


def _timing(*keys):
    return {key: LINK_TIMING_SCHEMA({key: "10ms"})[key] for key in keys}


def test_p300_takes_all_four():
    config = {CONF_PROTOCOL: "P300", CONF_LINK_TIMING: _timing(*P300_POINTS)}
    assert _validate_link_timing(config) == config


def test_kw_takes_its_two():
    config = {CONF_PROTOCOL: "KW", CONF_LINK_TIMING: _timing(CONF_ENQ_TO_SEND, CONF_ANSWER_TO_NEXT)}
    assert _validate_link_timing(config) == config


@pytest.mark.parametrize("key", [CONF_ENQ_TO_SYNC, CONF_FRAME_TO_ACK])
def test_kw_refuses_the_p300_points(key):
    with pytest.raises(cv.Invalid, match="P300") as info:
        _validate_link_timing({CONF_PROTOCOL: "KW", CONF_LINK_TIMING: _timing(key)})
    assert info.value.path == [CONF_LINK_TIMING, key]


def test_gwg_takes_its_two():
    config = {CONF_PROTOCOL: "GWG", CONF_LINK_TIMING: _timing(CONF_ENQ_TO_SEND, CONF_ACK_TO_TELEGRAM)}
    assert _validate_link_timing(config) == config


@pytest.mark.parametrize("key", [CONF_ANSWER_TO_NEXT, CONF_ENQ_TO_SYNC, CONF_FRAME_TO_ACK])
def test_gwg_refuses_the_others(key):
    with pytest.raises(cv.Invalid, match="P300 link; the GWG link has none") as info:
        _validate_link_timing({CONF_PROTOCOL: "GWG", CONF_LINK_TIMING: _timing(key)})
    assert info.value.path == [CONF_LINK_TIMING, key]


@pytest.mark.parametrize("protocol", ["KW", "P300"])
def test_only_gwg_takes_ack_to_telegram(protocol):
    with pytest.raises(cv.Invalid, match="of the GWG link") as info:
        _validate_link_timing({CONF_PROTOCOL: protocol, CONF_LINK_TIMING: _timing(CONF_ACK_TO_TELEGRAM)})
    assert info.value.path == [CONF_LINK_TIMING, CONF_ACK_TO_TELEGRAM]


@pytest.mark.parametrize("protocol", ["P300", "GWG", "KW"])
def test_without_it_every_protocol_passes(protocol):
    _validate_link_timing({CONF_PROTOCOL: protocol})
