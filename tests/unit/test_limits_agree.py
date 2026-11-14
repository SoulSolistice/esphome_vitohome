"""Size limits written down in more than one place hold the same value.

Config validation (components/vitohome/*.py), the catalog generator
(scripts/vitocatalog/constants.py) and the C++ hub each carry their own copy of
a limit. If the Python copy is the larger one, a config validates that the hub
then refuses at runtime; if the C++ copy is, the generator holds back entities
the hub would read. The C++ side checks its caps against the engine buffers
with a static_assert (vitohome.h); this checks the Python copies against it.
"""

import os
import pathlib
import re
import sys

import pytest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_SCRIPTS = os.path.join(_REPO_ROOT, "scripts")
for _p in (_SCRIPTS, _REPO_ROOT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from vitocatalog import constants as generator  # noqa: E402

from components.vitohome import MAX_P300_READ_LENGTH, MAX_RMW_BLOCK, RPC_PREFIX_MAX  # noqa: E402
from components.vitohome.text_sensor import MAX_TEXT_BLOCK_LENGTH  # noqa: E402

_COMPONENT = pathlib.Path(_REPO_ROOT) / "components" / "vitohome"


def _cpp_constant(header: str, name: str) -> int:
    text = (_COMPONENT / header).read_text(encoding="utf-8")
    match = re.search(rf"constexpr\s+\w+\s+{name}\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*;", text)
    assert match is not None, f"{name} not found in {header}"
    return int(match.group(1), 0)


@pytest.mark.parametrize(
    ("python_value", "header", "cpp_name"),
    [
        # The longest entity read is also the longest raw-console read, so any
        # block an entity reads can be tried in the console first.
        (MAX_P300_READ_LENGTH, "vitohome.h", "RAW_READ_MAX"),
        (MAX_RMW_BLOCK, "vitohome.h", "MAX_RMW_BLOCK"),
        (RPC_PREFIX_MAX, "vito_entity.h", "RPC_PREFIX_MAX"),
    ],
)
def test_component_limit_matches_the_hub(python_value, header, cpp_name):
    assert python_value == _cpp_constant(header, cpp_name)


@pytest.mark.parametrize("name", ["MAX_P300_READ_LENGTH", "MAX_TEXT_BLOCK_LENGTH", "MAX_RMW_BLOCK", "RPC_PREFIX_MAX"])
def test_generator_limit_matches_the_component(name):
    component = {
        "MAX_P300_READ_LENGTH": MAX_P300_READ_LENGTH,
        "MAX_TEXT_BLOCK_LENGTH": MAX_TEXT_BLOCK_LENGTH,
        "MAX_RMW_BLOCK": MAX_RMW_BLOCK,
        "RPC_PREFIX_MAX": RPC_PREFIX_MAX,
    }
    assert getattr(generator, name) == component[name]


def test_every_limit_fits_the_engine_buffers():
    max_payload = _cpp_constant("optolink/constants.h", "MAX_PAYLOAD")
    for value in (MAX_P300_READ_LENGTH, MAX_TEXT_BLOCK_LENGTH, MAX_RMW_BLOCK, RPC_PREFIX_MAX):
        assert value <= max_payload
