"""The declared minimum ESPHome version and the release CI builds against.

python -m pytest tests/unit/test_esphome_version.py -q
"""

import os
import re
import sys

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

from components import vitohome  # noqa: E402


def _pinned_esphome():
    with open(os.path.join(_REPO_ROOT, "requirements.txt"), encoding="utf-8") as f:
        m = re.search(r"^esphome==(\S+)$", f.read(), re.MULTILINE)
    assert m, "requirements.txt pins no esphome version"
    return m.group(1)


def test_ci_builds_against_the_declared_feature_release():
    # CI's pin must satisfy the minimum, and sit in the same feature release:
    # a minimum older than anything CI builds would be a promise nobody checks.
    pinned = vitohome._release(_pinned_esphome())
    minimum = vitohome._release(vitohome.MIN_ESPHOME_VERSION)
    assert pinned >= minimum
    assert pinned[:2] == minimum[:2]


def test_readme_names_the_minimum():
    with open(os.path.join(_REPO_ROOT, "README.md"), encoding="utf-8") as f:
        assert f"ESPHome {vitohome.MIN_ESPHOME_VERSION} or newer" in f.read()


def test_release_ignores_the_suffix():
    # A dev or beta build of the minimum satisfies it.
    assert vitohome._release("2026.9.0-dev") == (2026, 9, 0)
    assert vitohome._release("2026.9.0b1") == (2026, 9, 0)
