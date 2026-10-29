"""No callback runs while a hub queue's lock is held.

RingBuffer::consume_front_if() / move_first_to_front_if() call the caller's
lambda with the queue's mutex held -- on ESP32 a non-recursive FreeRTOS mutex.
Anything that can reach user code from there (publishing a state runs its
automations synchronously, an entity's error handler may publish or queue a
write) can take the same lock again and deadlock the task: A permanently
refused raw operation published its FAILED result from inside the raw queue's
consumer; a scan_result automation queueing the next read then blocked forever.

The hub is not host-compiled, so this is a structural check of its source:
every lambda handed to those calls is extracted and searched for calls that
lead out of the hub.
"""

import os
import re

_COMPONENT = os.path.join(os.path.dirname(__file__), "..", "..", "components", "vitohome")

_LOCKED_CALLS = ("consume_front_if", "move_first_to_front_if")

# Calls that can reach a callback, an automation, or another queue.
_FORBIDDEN = (
    r"publish",
    r"handle_\w*error",
    r"handle_\w*response",
    r"raw_handle_\w+",
    r"queue_raw_\w+",
    r"request_\w+",
    r"enqueue_\w+",
    r"_queue_\.(push|try|move|consume|reserve)",
    r"on_response_",
    r"on_error_",
)


def _lambda_bodies(source):
    for call in _LOCKED_CALLS:
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


def _strip_comments(body):
    body = re.sub(r"//[^\n]*", "", body)
    return re.sub(r'"(\\.|[^"\\])*"', '""', body)


def _sources():
    for name in sorted(os.listdir(_COMPONENT)):
        if name.endswith((".cpp", ".h")) and name != "ring_buffer.h":
            path = os.path.join(_COMPONENT, name)
            with open(path, encoding="utf-8") as f:
                yield name, f.read()


def test_the_check_finds_the_hub_lambdas():
    found = [(name, call) for name, src in _sources() for call, _, _ in _lambda_bodies(src)]
    assert ("vitohome.cpp", "consume_front_if") in found


def test_no_callback_under_a_queue_lock():
    offenders = []
    for name, src in _sources():
        for call, line, body in _lambda_bodies(src):
            code = _strip_comments(body)
            for pattern in _FORBIDDEN:
                hit = re.search(r"\b" + pattern + r"\s*\(", code) or re.search(pattern + r"\w*\(", code)
                if hit:
                    offenders.append(f"{name}:{line} {call}: {hit.group(0)}")
    assert not offenders, "callbacks inside a queue lock:\n" + "\n".join(offenders)


def test_the_check_would_catch_the_reviewed_deadlock():
    old = """
    this->raw_queue_.consume_front_if([this](const RawOp &operation) {
      if (!dispatched) {
        this->raw_handle_error_(optolink::OptolinkResult::ERROR);
        return true;
      }
      return true;
    });
    """
    bodies = list(_lambda_bodies(old))
    assert bodies and re.search(r"raw_handle_\w+\s*\(", _strip_comments(bodies[0][2]))


def test_every_dispatch_arms_the_watchdog_through_its_budget():
    # The watchdog budget follows the operation (arm_watchdog_());
    # a dispatch that set in_flight_started_ms_ directly
    # would run on a stale budget.
    with open(os.path.join(_COMPONENT, "vitohome.cpp"), encoding="utf-8") as f:
        src = f.read()
    assignments = re.findall(r"in_flight_started_ms_\s*=", src)
    assert len(assignments) == 1, "in_flight_started_ms_ is set only in arm_watchdog_()"
