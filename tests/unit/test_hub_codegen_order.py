"""The hub's to_code() sizes the entity registry before its first await.

A platform's to_code() resumes as soon as the hub variable exists. Had the hub
awaited anything first (the UART's variable, when the UART comes later in the
merged config, as with a package), the platforms' register_entity()
statements would land in main.cpp ahead of reserve_entities(), every one of
them refused: "entity registry full", the hub marked FAILED.
"""

import ast
import pathlib

_INIT = pathlib.Path(__file__).resolve().parents[2] / "components" / "vitohome" / "__init__.py"


def _to_code():
    tree = ast.parse(_INIT.read_text(encoding="utf-8"))
    for node in tree.body:
        if isinstance(node, ast.AsyncFunctionDef) and node.name == "to_code":
            return node
    raise AssertionError("no async to_code() in components/vitohome/__init__.py")


def _lines(fn, match):
    return sorted(n.lineno for n in ast.walk(fn) if match(n))


def test_reserve_entities_before_the_first_await():
    fn = _to_code()
    awaits = _lines(fn, lambda n: isinstance(n, ast.Await))
    reserves = _lines(
        fn,
        lambda n: isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute) and n.func.attr == "reserve_entities",
    )
    pvars = _lines(
        fn,
        lambda n: isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute) and n.func.attr == "new_Pvariable",
    )
    assert len(reserves) == 1, "reserve_entities() is emitted once"
    assert awaits, "to_code() awaits the component and the UART"
    assert pvars and pvars[0] < reserves[0] < awaits[0]
