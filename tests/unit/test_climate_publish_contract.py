"""The climate publishes only what the device confirmed (design_notes.md §5c).

VitoClimate::control() used to set the target temperature, preset and mode
and publish them as soon as the write was QUEUED; a write that then failed
left that state in Home Assistant until the next poll, and the call bypassed
the boot-time ready_() guard (external review, 2026-10-28). control() now
only queues; the read-back (or, with read_back off, the acknowledged write)
publishes. The climate is not host-compiled, so this checks its source.
"""

import os
import re

_SRC = os.path.join(os.path.dirname(__file__), "..", "..", "components", "vitohome", "vito_climate.cpp")


def _function_body(src, signature):
    start = src.index(signature)
    brace = src.index("{", start)
    depth = 0
    for i in range(brace, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return re.sub(r"//[^\n]*", "", src[brace : i + 1])
    raise AssertionError(signature)


def _src():
    with open(_SRC, encoding="utf-8") as f:
        return f.read()


def test_control_publishes_nothing():
    body = _function_body(_src(), "void VitoClimate::control(")
    assert "publish_state" not in body
    for field in ("this->target_temperature", "this->mode =", "set_custom_preset_", "clear_custom_preset_"):
        assert field not in body, field


def test_a_failed_write_changes_no_state():
    body = _function_body(_src(), "void VitoClimateChannel::handle_write_error(")
    assert "publish_state" not in body
    assert "parent_->on_" not in body


def test_an_acknowledged_write_publishes_only_without_read_back():
    body = _function_body(_src(), "void VitoClimateChannel::handle_write_response(")
    assert "wants_read_back()" in body
