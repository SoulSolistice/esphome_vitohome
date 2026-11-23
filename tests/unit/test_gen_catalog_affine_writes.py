"""A writable datapoint whose display is raw * factor + offset.

GWG_VWMS_36 "(85) Offset SPR 30L bottom" is MultOffset (value = raw * 0.5 - 16,
-16..16 K, writable). As a `noconv` number with the display's bounds, 0 K
staged raw 00 -- -16 K on the device. With a converter for the factor it is a
number with `value_offset`, raw 0..64 unsigned; without one the factor goes
to `scale:`. The raw signedness follows the raw range. Borders that are not
whole steps of the factor keep it a read-only sensor with filters.

The last test is the general guard over every shipped catalog: a number's
step and bounds (less value_offset) must be whole raw steps of its scale, or
the write rounds the value to another one.
"""

import glob
import os
import re
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_SCRIPTS = os.path.join(_REPO_ROOT, "scripts")
if _SCRIPTS not in sys.path:
    sys.path.insert(0, _SCRIPTS)

from vitocatalog.constants import _CONV_SCALE, PROTO_GWG  # noqa: E402
from vitocatalog.emitter import emit_entity  # noqa: E402
from vitocatalog.models import Event  # noqa: E402


def _event(**over):
    kw = {
        "id": "t",
        "name": "GWG BEMK85VerschiebungSPR30Lunten",
        "tech": "GWG_BEMK85VerschiebungSPR30Lunten",
        "address": 0x001F,
        "conversion": "MultOffset",
        "access_type": 3,
        "block_length": 1,
        "byte_length": 1,
        "byte_position": 0,
        "bit_length": 0,
        "bit_position": 0,
        "lower": "-16",
        "upper": "16",
        "stepping": "0.5",
        "unit": "K",
        "fc_read": "EEPROM_READ",
        "fc_write": "EEPROM_WRITE",
        "conv_factor": 0.5,
        "conv_offset": -16.0,
    }
    kw.update(over)
    return Event(**kw)


def _keys(lines):
    return [line.strip() for line in lines]


def test_writable_multoffset_is_a_number_with_value_offset():
    platform, lines = emit_entity(_event(), "full", PROTO_GWG)
    assert platform == "number"
    text = _keys(lines)
    assert "converter: div2" in text
    assert "value_offset: -16" in text
    assert any(k.startswith("signed: false") for k in text)
    assert {"min_value: -16", "max_value: 16", "step: 0.5"} <= set(text)
    assert not any(k.startswith(("- multiply", "- offset")) for k in text)


def test_writable_multoffset_without_a_converter_is_a_number_with_scale():
    platform, lines = emit_entity(_event(conv_factor=0.25, conv_offset=4.0), "full", PROTO_GWG)
    assert platform == "number"
    text = _keys(lines)
    assert "scale: 0.25" in text
    assert "value_offset: 4" in text
    assert not any(k.startswith(("converter", "- multiply", "- offset")) for k in text)
    assert {"min_value: -16", "max_value: 16", "step: 0.5"} <= set(text)


def test_borders_between_raw_steps_keep_it_a_sensor():
    # step 0.5 is 5/3 raw steps of 0.3: the borders contradict the factor.
    platform, lines = emit_entity(_event(conv_factor=0.3, conv_offset=0.0), "full", PROTO_GWG)
    assert platform == "sensor"
    text = _keys(lines)
    assert "- multiply: 0.3" in text
    assert "step 0.5 is not a whole number of raw steps (0.3)" in "\n".join(lines)
    assert not any(k.startswith(("scale", "min_value", "value_offset")) for k in text)


def test_a_step_below_the_raw_step_is_raised_to_it():
    # Mult10 rows with step 1: the frontend would offer 1, 2, ... that the write rounds.
    ev = _event(conversion="Mult10", conv_factor=None, conv_offset=None, lower="0", upper="1000", stepping="1")
    platform, lines = emit_entity(ev, "full", PROTO_GWG)
    assert platform == "number"
    text = _keys(lines)
    assert {"converter: mult10", "min_value: 0", "max_value: 1000", "step: 10"} <= set(text)
    assert "step 1 is below the raw step 10 -> step 10" in "\n".join(lines)


def test_a_lower_border_between_raw_steps_is_read_only():
    ev = _event(conversion="Mult10", conv_factor=None, conv_offset=None, lower="1", upper="1000", stepping="10")
    platform, lines = emit_entity(ev, "full", PROTO_GWG)
    assert platform == "sensor"
    assert "lower border 1 is between two raw steps (10) -> read-only" in "\n".join(lines)


def test_a_two_byte_scale_number():
    # Vitotwin_5000_Nennsollwert: raw * 1/64, 8..65 step 1 -> raw 512..4160.
    ev = _event(
        address=0x5000,
        block_length=2,
        byte_length=2,
        lower="8",
        upper="65",
        stepping="1",
        conv_factor=0.015625,
        conv_offset=0.0,
        fc_read="Virtual_READ",
        fc_write="Virtual_WRITE",
    )
    platform, lines = emit_entity(ev, "full")
    assert platform == "number"
    text = _keys(lines)
    assert {"scale: 0.015625", "length: 2", "min_value: 8", "max_value: 65", "step: 1"} <= set(text)
    assert not any(k.startswith(("converter", "value_offset", "signed")) for k in text)


def test_its_raw_range_is_unsigned():
    # raw = (value + 16) / 0.5 = 0..64: not signed, whatever the display border.
    _, lines = emit_entity(_event(), "full", PROTO_GWG)
    assert "signed: true" not in _keys(lines)


def test_a_negative_raw_range_stays_signed():
    # value = raw * 0.25 + 4, -16..16: raw -80..48 -- signed. (A plain factor
    # such as 0.5 would map to a preset converter instead of filters.)
    _, lines = emit_entity(_event(conv_factor=0.25, conv_offset=4.0), "full", PROTO_GWG)
    assert "signed: true" in _keys(lines)


def test_a_writable_unmapped_conversion_is_read_only():
    platform, _ = emit_entity(
        _event(conversion="SomeUnknownConversion", conv_factor=None, conv_offset=None, lower="0", upper="100", stepping="1"),
        "full",
        PROTO_GWG,
    )
    assert platform != "number"


def test_a_writable_identity_stays_a_number():
    platform, _ = emit_entity(
        _event(conversion="NoConversion", conv_factor=None, conv_offset=None, lower="0", upper="100", stepping="1"),
        "full",
        PROTO_GWG,
    )
    assert platform == "number"


_CATALOGS = sorted(glob.glob(os.path.join(_REPO_ROOT, "example", "catalogs", "*", "*.yaml")))


def _number_blocks(text):
    """Every `- platform: vitohome` block under the top-level `number:` key."""
    section = None
    block = None
    for line in text.splitlines():
        top = re.match(r"^([a-z_]+):\s*$", line)
        if top:
            if block is not None and section == "number":
                yield block
            section, block = top.group(1), None
            continue
        if re.match(r"^  - platform: vitohome", line):
            if block is not None and section == "number":
                yield block
            block = {}
            continue
        if block is not None:
            m = re.match(r"^    ([a-z_]+): (.*?)\s*(#.*)?$", line)
            if m:
                block[m.group(1)] = m.group(2).strip('"')
    if block is not None and section == "number":
        yield block


@pytest.mark.skipif(not _CATALOGS, reason="no shipped catalogs")
def test_shipped_numbers_sit_on_whole_raw_steps():
    offenders = []
    for path in _CATALOGS:
        with open(path, encoding="utf-8") as f:
            text = f.read()
        for block in _number_blocks(text):
            scale = float(block["scale"]) if "scale" in block else _CONV_SCALE[block.get("converter", "noconv")]
            offset = float(block.get("value_offset", 0))
            for key in ("min_value", "max_value", "step"):
                value = block.get(key)
                if value is None:
                    continue
                steps = (float(value) - (0 if key == "step" else offset)) / scale
                if abs(steps - round(steps)) > 1e-6:
                    offenders.append(f"{os.path.relpath(path, _REPO_ROOT)}: {block.get('id')} {key}={value}")
    assert not offenders, "numbers with bounds between raw steps:\n" + "\n".join(offenders[:20])
