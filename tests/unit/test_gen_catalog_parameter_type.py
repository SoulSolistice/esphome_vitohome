"""Vitosoft ParameterType as a second signal for signedness (gen_catalog).

The component decodes div2/div10 as signed by default. For a datapoint Vitosoft
declares unsigned (Byte/Int/Int4) that is wrong above half the range:
GWG_Kesseltemperatur (0x0070, 1 byte, Div2, Int, 0..127.5 degC) at 70 degC is
raw 0x8C, published as -58 degC. The generator now emits `signed: false` -- but
only when the lower border agrees (present and >= 0), because ParameterType
alone contradicts the borders on 27 rows, among them the outdoor sensor
ADC_IstTemperaturwert_ATSV333MW1~0x0800 (Int, -60..60).

    python -m pytest tests/unit/test_gen_catalog_parameter_type.py -q
"""

import os
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_SCRIPTS = os.path.join(_REPO_ROOT, "scripts")
for _p in (_SCRIPTS, _REPO_ROOT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from vitocatalog.constants import PROTO_GWG  # noqa: E402
from vitocatalog.emitter import emit_entity  # noqa: E402
from vitocatalog.loader import _load_access  # noqa: E402
from vitocatalog.models import Event  # noqa: E402


def _ev(**over):
    """GWG_Kesseltemperatur-shaped: read-only, 1 byte, Div2, Int, 0..127.5."""
    kw = {
        "id": "t",
        "name": "GWG Kesseltemperatur",
        "tech": "GWG_Kesseltemperatur",
        "address": 0x0070,
        "conversion": "Div2",
        "access_type": 1,
        "block_length": 1,
        "byte_length": 1,
        "byte_position": 0,
        "bit_length": 0,
        "bit_position": 0,
        "lower": "0",
        "upper": "127.5",
        "stepping": "0.5",
        "unit": "°C",
        "fc_read": "Physical_READ",
        "fc_write": "",
        "parameter_type": "Int",
    }
    kw.update(over)
    return Event(**kw)


def _signed_lines(lines):
    return [ln.strip() for ln in lines if ln.strip().startswith("signed:")]


def test_unsigned_div2_temperature_gets_signed_false():
    platform, lines = emit_entity(_ev(), "full", PROTO_GWG)
    assert platform == "sensor"
    assert "  converter: div2" in lines
    signed = _signed_lines(lines)
    assert len(signed) == 1 and signed[0].startswith("signed: false")
    assert "ParameterType Int" in signed[0]


@pytest.mark.parametrize(
    "over",
    [
        # ADC_IstTemperaturwert_ATSV333MW1~0x0800: declared Int, bordered -60..60
        # -- the contradiction that makes ParameterType unusable on its own.
        {"address": 0x0800, "conversion": "Div10", "byte_length": 2, "block_length": 2, "lower": "-60", "upper": "60"},
        # TiefpassTemperaturwert_ATS~0x5525 (VScotHO1_72): SInt, -60..60.
        {"address": 0x5525, "conversion": "Div10", "byte_length": 2, "block_length": 2, "parameter_type": "SInt"},
        # No borders at all (210 such rows, mostly raw ADC temperatures): keep the default.
        {"lower": "", "upper": "", "stepping": ""},
        # A signed type keeps the default even with a non-negative border.
        {"parameter_type": "SByte"},
        # HighByteFirst also changes byte order; not modelled, so not touched.
        {"parameter_type": "IntHighByteFirst"},
        # Not a signed-by-default converter: nothing to override.
        {"conversion": "Div100"},
    ],
)
def test_default_kept(over):
    _, lines = emit_entity(_ev(**over), "full", PROTO_GWG)
    assert not any(s.startswith("signed: false") for s in _signed_lines(lines))


def test_negative_border_still_emits_signed_true_on_an_unsigned_converter():
    # Div100 is unsigned by default; a negative border still makes it signed.
    ev = _ev(conversion="Div100", byte_length=2, block_length=2, lower="-30", upper="50")
    _, lines = emit_entity(ev, "full", PROTO_GWG)
    assert _signed_lines(lines) == ["signed: true"]


def test_writable_unsigned_div2_gets_its_real_bounds():
    # GWG_Trinkwasser_SolltemperaturBEM (EEPROM 0x0011, Byte, Div2, 10..90):
    # 90 degC is raw 180, which does not fit a SIGNED byte, so the number fell
    # back to the 0/0/1 placeholders. Unsigned, the real borders fit.
    ev = _ev(
        name="GWG Trinkwasser SolltemperaturBEM",
        tech="GWG_Trinkwasser_SolltemperaturBEM",
        address=0x0011,
        access_type=3,
        fc_read="EEPROM_READ",
        fc_write="EEPROM_WRITE",
        parameter_type="Byte",
        lower="10",
        upper="90",
        stepping="0.5",
    )
    platform, lines = emit_entity(ev, "full", PROTO_GWG)
    assert platform == "number"
    assert any(s.startswith("signed: false") for s in _signed_lines(lines))
    assert "  min_value: 10" in lines and "  max_value: 90" in lines and "  step: 0.5" in lines


def test_loader_reads_parameter_type(tmp_path):
    xml = tmp_path / "ecnEventType.xml"
    xml.write_text(
        "<root><EventType><ID>GWG_Kesseltemperatur~0x0070</ID><Address>0x0070</Address>"
        "<Parameter>Int</Parameter></EventType></root>",
        encoding="utf-8",
    )
    acc = _load_access(str(xml))
    assert acc["GWG_Kesseltemperatur~0x0070"]["parameter_type"] == "Int"
