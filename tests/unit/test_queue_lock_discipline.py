"""No callback runs while a hub queue's lock is held.

A RingBuffer call locks the queue for its own duration -- on ESP32 a
non-recursive FreeRTOS mutex. Anything that can reach user code from inside
that lock (publishing a state runs its automations synchronously, an entity's
error handler may publish or queue a write) can take the same lock again and
deadlock the task: a permanently refused raw operation published its FAILED
result from inside the raw queue's consumer; a scan_result automation queueing
the next read then blocked forever.

The hub's dispatch no longer runs under a lock at all: it looks at the head of
a lane (try_front), hands it to the engine, and removes it (drop_front). The
locked hand-off that caused the deadlock, consume_front_if(), is gone, and
must stay gone. One call still takes a caller's lambda under the lock,
move_first_to_front_if(); its predicate must stay free of calls that lead out
of the hub.

The hub is not host-compiled, so this is a structural check of its source.

Run:
    python -m pytest tests/unit/test_queue_lock_discipline.py -q
"""

import os
import re

_COMPONENT = os.path.join(os.path.dirname(__file__), "..", "..", "components", "vitohome")

# RingBuffer calls that run a caller's lambda with the queue's mutex held.
_LOCKED_CALLS = ("move_first_to_front_if",)

# Calls that publish, re-enter a queue, or reach an entity's handlers.
_FORBIDDEN = (
    r"publish",
    r"handle_\w*error",
    r"handle_\w*response",
    r"raw_handle_\w+",
    r"queue_raw_\w+",
    r"request_\w+",
    r"enqueue_\w+",
    r"_queue_\.(push|try|move|drop|remove|reserve)",
    r"on_response_",
    r"on_error_",
)


def _lambda_bodies(source, calls=_LOCKED_CALLS):
    """Yield (call, line, body) for every lambda handed to a locked call."""
    for call in calls:
        for m in re.finditer(rf"\b{call}\(", source):
            start = source.find("{", m.end())
            depth = 0
            for i in range(start, len(source)):
                if source[i] == "{":
                    depth += 1
                elif source[i] == "}":
                    depth -= 1
                    if depth == 0:
                        line = source.count("\n", 0, m.start()) + 1
                        yield call, line, source[start : i + 1]
                        break


_STRING_OR_COMMENT = re.compile(r'"(?:\\.|[^"\\\n])*"|//[^\n]*')


def _strip_comments(body):
    """Drop // comments and empty string literals, in one left-to-right pass.

    One pass, not two: a ``//`` inside a string (a URL) is not a comment, and a
    quote inside a comment does not open a string. Doing either first corrupts
    the other for the rest of the file.
    """
    return _STRING_OR_COMMENT.sub(lambda m: '""' if m.group(0).startswith('"') else "", body)


def _sources(include_ring_buffer=False):
    for name in sorted(os.listdir(_COMPONENT)):
        if name.endswith((".cpp", ".h")) and (include_ring_buffer or name != "ring_buffer.h"):
            path = os.path.join(_COMPONENT, name)
            with open(path, encoding="utf-8") as f:
                yield name, f.read()


def _offenders(source, name="<source>", calls=_LOCKED_CALLS):
    found = []
    for call, line, body in _lambda_bodies(source, calls):
        code = _strip_comments(body)
        for pattern in _FORBIDDEN:
            hit = re.search(r"\b" + pattern + r"\s*\(", code) or re.search(pattern + r"\w*\(", code)
            if hit:
                found.append(f"{name}:{line} {call}: {hit.group(0)}")
    return found


def test_the_locked_hand_off_is_gone():
    # Comments may still name it (they say why it went); code must not call or
    # define it.
    for name, src in _sources(include_ring_buffer=True):
        code = _strip_comments(src)
        assert "consume_front_if" not in code, f"{name} still uses consume_front_if()"


def test_the_hub_dispatches_by_peek_and_drop():
    with open(os.path.join(_COMPONENT, "vitohome.cpp"), encoding="utf-8") as f:
        code = _strip_comments(f.read())
    for lane in ("raw_queue_", "write_queue_", "read_queue_"):
        assert re.search(rf"\b{lane}\.try_front\(", code), f"{lane} is not peeked with try_front()"
        assert re.search(rf"\b{lane}\.drop_front\(", code), f"{lane} is not removed with drop_front()"


def test_the_check_finds_the_hub_lambdas():
    found = [(name, call) for name, src in _sources() for call, _, _ in _lambda_bodies(src)]
    assert ("vitohome.cpp", "move_first_to_front_if") in found


def test_no_callback_under_a_queue_lock():
    offenders = [o for name, src in _sources() for o in _offenders(src, name)]
    assert not offenders, "callbacks inside a queue lock:\n" + "\n".join(offenders)


def test_the_check_would_catch_the_reviewed_deadlock():
    # The shape of the bug, as it was written against consume_front_if().
    old = """
    this->raw_queue_.consume_front_if([this](const RawOp &operation) {
      if (!dispatched) {
        this->raw_handle_error_(optolink::OptolinkResult::ERROR);
        return true;
      }
      return true;
    });
    """
    assert _offenders(old, calls=("consume_front_if",))


def test_every_dispatch_arms_the_watchdog_through_its_budget():
    with open(os.path.join(_COMPONENT, "vitohome.cpp"), encoding="utf-8") as f:
        src = f.read()
    assignments = re.findall(r"in_flight_started_ms_\s*=", src)
    assert len(assignments) == 1, "in_flight_started_ms_ is set only in arm_watchdog_()"


def test_the_in_flight_state_has_one_writer():
    # The operation in flight and its entity are one fact (vitohome.h, OpType):
    # written together by set_in_flight_() / take_in_flight_() in the header,
    # never assigned piecemeal in the hub's source.
    with open(os.path.join(_COMPONENT, "vitohome.cpp"), encoding="utf-8") as f:
        code = _strip_comments(f.read())
    assert not re.search(r"\bin_flight_op_\s*=[^=]", code), "in_flight_op_ assigned outside set/take_in_flight_()"
    assert not re.search(r"\bin_flight_\s*=[^=]", code), "in_flight_ assigned outside set/take_in_flight_()"
    for gone in ("ident_in_flight_", "raw_in_flight_"):
        assert gone not in code, f"{gone} is back: the bus has ONE owner field"
