"""Intervals the hub's schedules can hold.

Deadlines compare through a signed 32-bit millisecond difference
(poll_schedule.h, vito_clock.cpp), so an interval of 2^31 ms (24.8 days) or
more looks already due and runs on every hub tick, and ESPHome's `never`
(0xFFFFFFFF ms) did the same -- polled on every tick instead of never
(external review, 2026-10-28). The config refuses the first and passes
`never` through to POLL_NEVER.
"""

import os
import re
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

import esphome.config_validation as cv  # noqa: E402

from components.vitohome import POLL_INTERVAL_MAX_MS, POLL_NEVER_MS, TIME_SYNC_SCHEMA, poll_update_interval  # noqa: E402

_COMPONENT = os.path.join(_REPO_ROOT, "components", "vitohome")


def _ms(value):
    return int(value.total_milliseconds)


def test_never_passes_through_as_the_sentinel():
    assert _ms(poll_update_interval("never")) == POLL_NEVER_MS


@pytest.mark.parametrize("value", ["60s", "1h", "24d"])
def test_representable_intervals_are_accepted(value):
    assert _ms(poll_update_interval(value)) <= POLL_INTERVAL_MAX_MS


@pytest.mark.parametrize("value", ["25d", "30d", "365d"])
def test_longer_intervals_are_refused(value):
    with pytest.raises(cv.Invalid, match="24 days"):
        poll_update_interval(value)


def test_time_sync_interval_is_bounded_too():
    with pytest.raises(cv.Invalid, match="24 days"):
        TIME_SYNC_SCHEMA({"interval": "30d"})
    TIME_SYNC_SCHEMA({"interval": "7d"})


def test_the_limits_match_the_cpp_constants():
    with open(os.path.join(_COMPONENT, "poll_schedule.h"), encoding="utf-8") as f:
        header = f.read()
    assert f"POLL_NEVER = 0x{POLL_NEVER_MS:08X}u" in header
    assert f"POLL_INTERVAL_MAX_MS = 0x{POLL_INTERVAL_MAX_MS:08X}u" in header


def test_no_platform_uses_the_unbounded_validator():
    # Every per-datapoint update_interval goes through poll_update_interval.
    offenders = []
    for name in sorted(os.listdir(_COMPONENT)):
        if not name.endswith(".py"):
            continue
        with open(os.path.join(_COMPONENT, name), encoding="utf-8") as f:
            src = f.read()
        hits = re.finditer(r"CONF_UPDATE_INTERVAL\)\s*:\s*cv\.update_interval", src)
        offenders.extend(f"{name}:{src.count(chr(10), 0, m.start()) + 1}" for m in hits)
    assert not offenders, offenders
