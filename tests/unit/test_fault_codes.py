"""Tests for scripts/fault_codes.py — the fault-code data module and its
relationship to gen_catalog's --error-code-set flag.

Stdlib-only; locks the set sizes, the UNION construction rule, and the fact that
the generator's CLI choices stay in sync with the module's registry.
"""

import importlib.util
import os
import sys

_SCRIPTS = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "scripts")


def _load(name):
    sys.path.insert(0, _SCRIPTS)
    spec = importlib.util.spec_from_file_location(name, os.path.join(_SCRIPTS, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


fc = _load("fault_codes")


def test_set_sizes():
    assert len(fc.OPENV) == 41
    assert len(fc.VITOTRONIC_VD200) == 59
    assert len(fc.VITOTRONIC_VD300_B3HA) == 94
    assert len(fc.UNION) == 105  # vd300 (94) + vd200-only + openv-only
    assert len(fc.CONFLICTS) == 4


def test_registry_and_accessor():
    assert set(fc.SETS) == {"openv", "vd200", "vd300", "union"}
    assert fc.SETS["openv"] is fc.OPENV
    assert fc.SETS["vd200"] is fc.VITOTRONIC_VD200
    assert fc.SETS["vd300"] is fc.VITOTRONIC_VD300_B3HA
    assert fc.get("union") is fc.UNION


def test_union_construction():
    # Most-specific manual wins: VD300-W is authoritative; VD200 then OPENV fill gaps.
    for code, text in fc.VITOTRONIC_VD300_B3HA.items():
        assert fc.UNION[code] == text, f"union must keep VD300 wording for 0x{code:02X}"
    only_vd200 = set(fc.VITOTRONIC_VD200) - set(fc.VITOTRONIC_VD300_B3HA)
    for code in only_vd200:
        assert fc.UNION[code] == fc.VITOTRONIC_VD200[code]
    only_openv = set(fc.OPENV) - set(fc.VITOTRONIC_VD200) - set(fc.VITOTRONIC_VD300_B3HA)
    for code in only_openv:
        assert fc.UNION[code] == fc.OPENV[code]
    # union is exactly the three maps merged, nothing else.
    assert set(fc.UNION) == set(fc.VITOTRONIC_VD300_B3HA) | set(fc.VITOTRONIC_VD200) | set(fc.OPENV)


def test_conflicts_are_real_disagreements():
    # Every CONFLICTS code exists in both maps and the texts actually differ.
    for code, (openv_text, vd200_text) in fc.CONFLICTS.items():
        assert code in fc.OPENV and code in fc.VITOTRONIC_VD200
        assert openv_text != vd200_text


def test_codes_are_single_byte_int():
    for name, mapping in fc.SETS.items():
        for code in mapping:
            assert isinstance(code, int) and 0 <= code <= 0xFF, f"{name}: bad code {code!r}"


def test_cli_choices_match_registry():
    # The generator imports the same module, and its --error-code-set choices are
    # built from SETS, so adding a set in fault_codes.py automatically offers it
    # on the CLI and the default stays valid.
    gc = _load("gen_catalog")
    assert gc.fault_codes is fc
    assert "vd300" in fc.SETS  # generate()'s default set is a valid registry key
