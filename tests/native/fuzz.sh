#!/usr/bin/env bash
# Coverage-guided fuzzing campaign. Human-initiated, one target or all of them
# in turn.
#
# Not a gate and NOT run by CI. CI executes the same targets in their
# deterministic replay builds (build_and_run.sh and build_and_run_protocols.sh
# -> ./fuzz_<target>), which walk the committed seed corpora in a fraction of a
# second. What lives here is the open-ended search: it runs for as long as you
# let it, its result depends on the seed and the clock, and a campaign that
# finds nothing is not evidence worth spending a CI minute on. Each target is
# one source file compiled both ways, so the oracles CI enforces and the
# oracles a campaign enforces cannot drift apart.
#
# Requires clang: -fsanitize=fuzzer is a compiler-rt feature with no GCC
# equivalent, which is why the CI-side builds stay on g++ and this one does
# not. Ubuntu 24.04's clang 18 is sufficient.
#
#   bash fuzz.sh                         # list the targets
#   bash fuzz.sh parser_vs2              # 5 minutes, one worker per core
#   bash fuzz.sh decode 3600             # an hour
#   bash fuzz.sh decode 300 2            # 5 minutes, 2 workers
#   bash fuzz.sh all 3600                # every target for an hour each
#   KEEP_GOING=1 bash fuzz.sh all 14400  # overnight: findings do not stop it
#
# MODES. By default every worker runs until its first finding, which suits a
# short campaign whose finding you look at right away. KEEP_GOING=1 runs
# libFuzzer's fork mode instead: crashes, timeouts and out-of-memory reports
# are written to findings/ and the search goes on, so a long run is not spent
# by one shallow defect. `all` runs every target in turn, SECONDS each, carries
# on past a target with findings and lists them at the end.
#
# STATE. Corpus and findings live in $FUZZ_DIR/<target>/, by default
# ~/.cache/vitohome-fuzz, which survives a reboot (WSL clears /tmp), so the
# next campaign starts from the corpus the last one reached. Each run's output
# is kept in $FUZZ_DIR/<target>/log/.
#
# TARGETS
#   parser_vs2   ParserVS2::parse() -- the wire->payload state machine.
#                Saturates fast: the committed seeds already reach the coverage
#                ceiling at INITED, so a long campaign here buys little.
#   decode       the buffer-writing helpers in decode.h -- payload->text, plus
#                encode_schaltzeiten_day, the one input a PERSON types.
#   field        the value half of decode.h -- field extraction, scaling and
#                the encode inverses, with round-trip oracles.
#   raw_spec     raw_op.h's spec parser -- a typed line to an operation.
#   raw_engines  one generic telegram on each engine against a fuzzed
#                controller.
#   gwg_session  GWG sessions -- burst, EEPROM runs, ENQ delay, deadline and
#                the late-answer report against a fuzzed device.
#   kw_session   KW sessions on a KW link and over the P300 detour -- chain
#                window, GFA pause and protocol switches against a fuzzed
#                controller.
#
# WHAT TO DO WITH A FINDING. libFuzzer writes the reproducer to
# $FUZZ_DIR/<target>/findings/ and prints the path. First replay it against the
# CI build to confirm it is real and not an artefact of the fuzzer's own
# instrumentation -- the replay binaries take files as arguments:
#
#   cd tests/native && bash build_and_run.sh   # builds ./fuzz_<target>
#   ./fuzz_decode /path/to/crash-file
#
# Then minimise it (`-minimize_crash=1 -runs=100000 <file>`) and add the
# reduced bytes to the target's corpus header with a comment saying what it
# broke. That is the step that converts a campaign into a permanent gate; a
# crash file left behind is a finding that will be rediscovered from scratch
# next year. Delete it from findings/ once it is a seed.
set -euo pipefail

# Resolved before anything else so the script works from any cwd; the relative
# ROOT default below is then relative to tests/native, as in build_and_run.sh.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

TARGETS=(parser_vs2 decode field raw_spec raw_engines gwg_session kw_session)
TARGET="${1:-}"
SECS="${2:-300}"
WORKERS="${3:-$(nproc)}"
ROOT="${VITOHOME_ROOT:-../../components/vitohome}"
OPTO="$ROOT/optolink"
FUZZ_DIR="${FUZZ_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/vitohome-fuzz}"
KEEP_GOING="${KEEP_GOING:-0}"

if [ "$TARGET" = all ]; then
  failed=()
  for t in "${TARGETS[@]}"; do
    bash "$SCRIPT_DIR/fuzz.sh" "$t" "$SECS" "$WORKERS" || failed+=("$t")
  done
  echo
  if [ ${#failed[@]} -gt 0 ]; then
    echo "== all: findings or errors in: ${failed[*]} =="
    for t in "${failed[@]}"; do
      find "$FUZZ_DIR/$t/findings" -type f -printf '  %p (%s bytes)\n' 2>/dev/null || true
    done
    exit 1
  fi
  echo "== all: ${#TARGETS[@]} targets, no findings =="
  exit 0
fi

# target -> source, corpus header, and any extra translation units it links.
# decode.h is header-only and included relatively, so that target needs no
# extra sources; the parser links the engine's parser/packet/constants.
case "$TARGET" in
  parser_vs2)
    SRC="fuzz_parser_vs2.cpp"
    HEADER="fuzz_corpus_vs2.h"
    EXTRA=("$OPTO/protocol/vs2/parser_vs2.cpp" "$OPTO/protocol/vs2/packet_vs2.cpp" "$OPTO/constants.cpp")
    ;;
  decode)
    SRC="fuzz_decode.cpp"
    HEADER="fuzz_corpus_decode.h"
    EXTRA=()
    ;;
  field)
    SRC="fuzz_field.cpp"
    HEADER="fuzz_corpus_field.h"
    EXTRA=()
    ;;
  raw_spec)
    SRC="fuzz_raw_spec.cpp"
    HEADER="fuzz_corpus_raw_spec.h"
    EXTRA=()
    ;;
  raw_engines)
    SRC="fuzz_raw_engines.cpp"
    HEADER="fuzz_corpus_raw_engines.h"
    EXTRA=("$OPTO/constants.cpp" "$OPTO/protocol/vs2/vs2.cpp" "$OPTO/protocol/vs2/parser_vs2.cpp"
      "$OPTO/protocol/vs2/packet_vs2.cpp" "$OPTO/protocol/vs1/vs1.cpp" "$OPTO/protocol/vs1/packet_vs1.cpp"
      "$OPTO/protocol/gwg/gwg.cpp" "$OPTO/protocol/gwg/packet_gwg.cpp")
    ;;
  gwg_session)
    SRC="fuzz_gwg_session.cpp"
    HEADER="fuzz_corpus_gwg_session.h"
    EXTRA=("$OPTO/constants.cpp" "$OPTO/protocol/gwg/gwg.cpp" "$OPTO/protocol/gwg/packet_gwg.cpp")
    ;;
  kw_session)
    SRC="fuzz_kw_session.cpp"
    HEADER="fuzz_corpus_kw_session.h"
    EXTRA=("$OPTO/constants.cpp" "$OPTO/protocol/vs2/vs2.cpp" "$OPTO/protocol/vs2/parser_vs2.cpp"
      "$OPTO/protocol/vs2/packet_vs2.cpp" "$OPTO/protocol/vs1/vs1.cpp" "$OPTO/protocol/vs1/packet_vs1.cpp")
    ;;
  *)
    echo "usage: [KEEP_GOING=1] bash fuzz.sh <target|all> [seconds] [workers]" >&2
    echo "targets:" >&2
    echo "  parser_vs2   ParserVS2::parse() -- wire to payload" >&2
    echo "  decode       decode.h buffer writers -- payload to text" >&2
    echo "  field        decode.h field extraction and scaling -- payload to value and back" >&2
    echo "  raw_spec     raw_op.h spec parser -- a typed line to an operation" >&2
    echo "  raw_engines  generic telegrams -- one operation against a fuzzed controller" >&2
    echo "  gwg_session  GWG sessions -- up to six operations against a fuzzed device" >&2
    echo "  kw_session   KW and P300-detour sessions -- up to six operations against a fuzzed controller" >&2
    exit 2
    ;;
esac

if ! command -v clang++ >/dev/null 2>&1; then
  echo "fuzz.sh needs clang++ (-fsanitize=fuzzer has no GCC equivalent)." >&2
  echo "  sudo apt install clang llvm" >&2
  exit 1
fi

WORK="$FUZZ_DIR/$TARGET"
CORPUS="$WORK/corpus"
FINDINGS="$WORK/findings"
LOGS="$WORK/log"
mkdir -p "$CORPUS" "$FINDINGS" "$LOGS"
before=$(find "$FINDINGS" -type f | wc -l)

# Materialise the committed seeds as files. libFuzzer wants a directory; the
# corpora live in headers so they stay reviewable and survive the text-typed
# pre-commit hooks (see fuzz_corpus_vs2.h). This is the one place the two
# representations meet, and it is a pure export -- the header is the source of
# truth and is never written back to. Corpus entries the fuzzer discovers
# accumulate alongside them and are deliberately NOT committed: they are a
# cache that makes the next campaign start warm, not a gate.
python3 - "$CORPUS" "$SCRIPT_DIR/$HEADER" <<'PY'
import os, re, sys
out, header = sys.argv[1], sys.argv[2]
src = open(header).read()
n = 0
# kSeed_ (parser) and kDSeed_ (decode) share one shape.
for m in re.finditer(r"k[A-Za-z]*Seed_(\w+)\[\] = \{(.*?)\};", src, re.S):
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(2)))
    open(os.path.join(out, "seed_" + m.group(1)), "wb").write(data)
    n += 1
print(f"  {n} seeds exported to {out}")
PY

echo "== building the libFuzzer target '$TARGET' (clang) =="
clang++ -std=c++17 -g -Wall -Wextra -Werror \
  -DVITOHOME_FUZZ_LIBFUZZER \
  -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
  -I"$ROOT" -I"$OPTO" \
  "$SRC" ${EXTRA[@]+"${EXTRA[@]}"} \
  -o "$WORK/fuzz_$TARGET"

STAMP="$(date +%Y%m%d-%H%M%S)"
LOG="$LOGS/$STAMP.log"
if [ "$KEEP_GOING" = 1 ]; then
  MODE_ARGS=(-fork="$WORKERS" -ignore_crashes=1 -ignore_timeouts=1 -ignore_ooms=1)
  MODE="fork mode, findings do not stop it"
else
  MODE_ARGS=(-workers="$WORKERS" -jobs="$WORKERS")
  MODE="each worker stops at its first finding"
fi
echo "== campaign '$TARGET': ${SECS}s, ${WORKERS} worker(s), $MODE =="
echo "   corpus $CORPUS"
echo "   log    $LOG"
# Run from $WORK, not from the source tree: with -jobs, libFuzzer writes a
# fuzz-<N>.log per worker into the CURRENT directory. They are appended to the
# run's log afterwards. Every path handed to the binary is absolute.
cd "$WORK"
rm -f fuzz-*.log
# -timeout: every input runs a bounded script, so one taking 10 s is a loop
# that does not end, and is reported as a finding.
set +e
"$WORK/fuzz_$TARGET" "$CORPUS" \
  -artifact_prefix="$FINDINGS/" \
  -max_total_time="$SECS" \
  -timeout=10 \
  "${MODE_ARGS[@]}" \
  -print_final_stats=1 2>&1 | tee "$LOG"
rc=${PIPESTATUS[0]}
set -e
for f in fuzz-*.log; do
  [ -e "$f" ] || continue
  { echo "== $f =="; cat "$f"; } >>"$LOG"
  rm -f "$f"
done

found=$(find "$FINDINGS" -type f | wc -l)
cov=$(grep -o 'cov: [0-9]*' "$LOG" | awk '{print $2}' | sort -n | tail -1 || true)
echo
echo "'$TARGET': libFuzzer cov ${cov:-?}, corpus $(find "$CORPUS" -type f | wc -l) units."
if [ "$found" -gt 0 ]; then
  echo "FINDINGS: $found reproducer(s) in $FINDINGS ($((found - before)) new)"
  find "$FINDINGS" -type f -printf '  %p (%s bytes)\n'
  echo "Replay one against the CI build, minimise it, then promote it into $HEADER."
  exit 1
fi
echo "no findings."
exit "$rc"
