# Reference engines

A frozen copy of `components/vitohome/optolink/` (constants, interface,
protocol) as it stood before the engine redesign: the P300 engine with both
of its one-pass fixes, KW with the 4 s EOT interval and the 500 ms chain
window, GWG with the restart fix and the 80 ms burst window, and every
engine's begin() reading the clock.
The differential target (`fuzz_diff_engines.cpp`) runs each fuzz harness
against these and against the live engines and compares every pass: the
bytes written, the results and the events. A redesign stage passes when the
two agree, or when it changes the wire on purpose and the fixtures say so.

Made by `make_reference.py`, with one rule: `esphome::vitohome::optolink`
becomes `esphome::vitohome::optolink_ref`. The script refuses to run over an
existing copy. Apart from the namespace, the 18 copied files
equal the live ones at the branch point. `helpers.h` is a shim to the live clock
(`optolink_clock.h`), so both sides share one test clock, `logging.h` keeps
the logging silent, and `optolink.h` includes the engines only. The build files and `datapoint/`
are not copied. The files' headers name `THIRD_PARTY.md`, which is gone: what
it recorded is in `docs/*_protocol.md` and `docs/design_notes.md`, the
attribution in `NOTICE.md`.

Compiled only in the native tests. It outlived `optolink/` (stage 6): the
GWG changes of stage 5c are still measured against it. It goes once its
recorded outputs are kept as fixtures.
