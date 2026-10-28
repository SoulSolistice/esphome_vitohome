"""A writable datapoint whose display needs a transform the component cannot
encode must not become a `number` with the display's bounds.

External review, 2026-10-28: GWG_VWMS_36 "(85) Offset SPR 30L bottom" is
MultOffset (value = raw * 0.5 - 16, -16..16 K, writable). It shipped as a
`noconv` number with min -16, max 16, step 0.5 and `signed: true`, so 0 K
staged raw 00 -- -16 K on the device. Such datapoints are read-only sensors
with the transform as filters, and the raw signedness follows the raw range.

The last test is the general guard over every shipped catalog: a `noconv`
number writes its value as the raw register value, so its step and bounds
must be whole numbers.
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

import gen_catalog as gc  # noqa: E402


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
    return gc.Event(**kw)


def _keys(lines):
    return [line.strip() for line in lines]


def test_writable_multoffset_is_a_sensor_with_its_transform():
    platform, lines = gc.emit_entity(_event(), "full", gc.PROTO_GWG)
    assert platform == "sensor"
    text = _keys(lines)
    assert "- multiply: 0.5" in text
    assert "- offset: -16" in text
    assert not any(k.startswith(("min_value", "max_value", "step")) for k in text)


def test_its_raw_range_is_unsigned():
    # raw = (value + 16) / 0.5 = 0..64: not signed, whatever the display border.
    _, lines = gc.emit_entity(_event(), "full", gc.PROTO_GWG)
    assert "signed: true" not in _keys(lines)


def test_a_negative_raw_range_stays_signed():
    # value = raw * 0.25 + 4, -16..16: raw -80..48 -- signed. (A plain factor
    # such as 0.5 would map to a preset converter instead of filters.)
    _, lines = gc.emit_entity(_event(conv_factor=0.25, conv_offset=4.0), "full", gc.PROTO_GWG)
    assert "signed: true" in _keys(lines)


def test_a_writable_unmapped_conversion_is_read_only():
    platform, _ = gc.emit_entity(
        _event(conversion="SomeUnknownConversion", conv_factor=None, conv_offset=None, lower="0", upper="100", stepping="1"),
        "full",
        gc.PROTO_GWG,
    )
    assert platform != "number"


def test_a_writable_identity_stays_a_number():
    platform, _ = gc.emit_entity(
        _event(conversion="NoConversion", conv_factor=None, conv_offset=None, lower="0", upper="100", stepping="1"),
        "full",
        gc.PROTO_GWG,
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
def test_shipped_noconv_numbers_have_raw_bounds():
    offenders = []
    for path in _CATALOGS:
        with open(path, encoding="utf-8") as f:
            text = f.read()
        for block in _number_blocks(text):
            if block.get("converter", "noconv") != "noconv":
                continue
            for key in ("min_value", "max_value", "step"):
                value = block.get(key)
                if value is not None and float(value) != int(float(value)):
                    offenders.append(f"{os.path.relpath(path, _REPO_ROOT)}: {block.get('id')} {key}={value}")
    assert not offenders, "noconv numbers with fractional raw bounds:\n" + "\n".join(offenders[:20])
