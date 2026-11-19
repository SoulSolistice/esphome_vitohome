"""`vitohome.raw_op` -- queue_raw_op() as an automation action.

The action forwards its spec to the hub and has no C++ class of its own
(register_apply_action). The raw gates stay the hub's: the spec is checked and
refused at run time, as for a lambda's call.
"""

import os
import sys

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

from esphome import automation  # noqa: E402
import esphome.config_validation as cv  # noqa: E402

from components.vitohome import CONF_SPEC  # noqa: E402


def _entry():
    return automation.ACTION_REGISTRY["vitohome.raw_op"]


def test_the_action_is_registered_without_a_class():
    assert _entry().type_id is automation.ApplyAction


def test_the_spec_is_required_and_templatable():
    keys = {str(k): k for k in _entry().raw_schema.schema}
    assert isinstance(keys[CONF_SPEC], cv.Required)
    # A constant and a lambda both validate.
    validator = _entry().raw_schema.schema[keys[CONF_SPEC]]
    assert validator("gwg f 01 ~30 AE 16 01 04 / 2") == "gwg f 01 ~30 AE 16 01 04 / 2"
    assert isinstance(validator(cv.Lambda('return "kw r 7B 0800 2";')), cv.Lambda)
