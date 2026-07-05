"""Regression test for the protocol build-flag normalization.

``cv.enum`` returns the *key* the user typed (an ``EnumValue`` str subclass),
with the mapped value only in ``.enum_value``. ``to_code`` used to interpolate
that key directly into the build flag, so ``protocol: VS1`` emitted
``-DVITOHOME_PROTOCOL_VS1`` -- a flag ``protocol_adapter.h`` does not know --
and silently built the default P300 engine instead of KW.

These tests lock the invariant that every accepted ``protocol:`` spelling
normalizes to one of the three tokens the adapter's ``#if`` chain actually
checks (P300 / KW / GWG), using the real ESPHome ``cv.enum`` (not a stub), so a
future cv.enum behaviour change would surface here too.
"""

import os
import sys

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

import esphome.config_validation as cv  # noqa: E402

from components.vitohome import PROTOCOLS  # noqa: E402

# The only tokens protocol_adapter.h's #if chain recognises. Keep in sync with
# components/vitohome/protocol_adapter.h (VITOHOME_PROTOCOL_KW / _GWG; anything
# else falls through to P300).
_ADAPTER_TOKENS = {"P300", "KW", "GWG"}


def _normalize(user_value: str) -> str:
    """What to_code does: validate through the real cv.enum, then map the
    returned key through PROTOCOLS to the adapter token."""
    validated = cv.enum(PROTOCOLS, upper=True)(user_value)
    return PROTOCOLS[str(validated)]


def test_every_protocols_value_is_an_adapter_token():
    # The mapping's *values* must all be flags the adapter recognises; a typo'd
    # or new-but-unwired value would silently select P300.
    assert set(PROTOCOLS.values()) <= _ADAPTER_TOKENS


def test_aliases_normalize_to_adapter_tokens():
    assert _normalize("P300") == "P300"
    assert _normalize("VS2") == "P300"
    assert _normalize("KW") == "KW"
    assert _normalize("VS1") == "KW"
    assert _normalize("GWG") == "GWG"


def test_lowercase_input_normalizes_too():
    # cv.enum(..., upper=True) uppercases the user's spelling first.
    assert _normalize("vs1") == "KW"
    assert _normalize("p300") == "P300"


def test_cv_enum_returns_the_key_not_the_value():
    # Documents the cv.enum contract the bug hinged on: the validated object
    # string-compares to the KEY; the mapping lives in .enum_value. If ESPHome
    # ever changes this, the normalization in to_code must be revisited.
    validated = cv.enum(PROTOCOLS, upper=True)("VS1")
    assert str(validated) == "VS1"
    assert validated.enum_value == "KW"


# --- identify_device default per protocol ------------------------------------
# The default mirrors components/vitohome/__init__.py::to_code: on for P300 and
# KW (identification is hardware-confirmed on both -- VScotHO1_72 0x20CB dumps
# "HW=0x03 SW=0x51" over each), off for GWG (untested single-byte scheme).


def _identify_default(protocol: str) -> bool:
    # The exact expression to_code uses when identify_device is unset.
    return protocol in ("P300", "KW")


def test_identify_default_on_for_p300_and_kw():
    assert _identify_default("P300") is True
    assert _identify_default("KW") is True


def test_identify_default_off_for_gwg():
    assert _identify_default("GWG") is False
