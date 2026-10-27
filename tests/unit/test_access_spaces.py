"""The `access:` table (ACCESS_SPACES, __init__.py) against its C++ twin.

AddressSpace (components/vitohome/address_space.h) is where the hub turns an
`access:` value into a telegram; ACCESS_SPACES is where the config validation
decides which protocols have it and whether it is written. They are kept in
step by hand, so this checks that every value one side knows the other knows
too, and that the table's protocol and write rules match the C++ ones.
"""

import os
import re
import sys

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

from components.vitohome import ACCESS_SPACES, GWG_ACCESS_MODES  # noqa: E402

_HEADER = os.path.join(_REPO_ROOT, "components", "vitohome", "address_space.h")


def _cpp_space_names() -> set[str]:
    """The YAML names space_name() returns, from the header's source."""
    with open(_HEADER, encoding="utf-8") as fh:
        text = fh.read()
    body = text[text.index("constexpr const char *space_name(") :]
    return set(re.findall(r'return "([a-z_]+)";', body))


def test_every_cpp_space_has_a_table_row_and_back():
    cpp = _cpp_space_names()
    # PHYSICAL is OWN in C++ (space_name() gives "" for it); every other
    # value is one enumerator.
    assert cpp == set(ACCESS_SPACES) - {"physical"}


def test_protocols_match_space_available():
    assert ACCESS_SPACES["rpc"].protocols == ("P300",)
    assert set(ACCESS_SPACES["gfa"].protocols) == {"KW", "P300"}
    for mode in GWG_ACCESS_MODES:
        assert ACCESS_SPACES[mode].protocols == ("GWG",)


def test_writable_matches_space_writable():
    written = {name for name, space in ACCESS_SPACES.items() if space.writable}
    assert written == {"physical", "virtual", "eeprom", "xram", "port", "be"}
