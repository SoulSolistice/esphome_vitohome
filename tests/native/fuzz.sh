#!/usr/bin/env bash
# Coverage-guided fuzzing campaign. Human-initiated, one target at a time.
#
# Not a gate and NOT run by CI. CI executes the same targets in their
# deterministic replay builds (build_and_run.sh -> fuzz_parser_vs2,
# fuzz_decode), which walk the committed seed corpora in a fraction of a
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
#   bash fuzz.sh                      # list the targets
#   bash fuzz.sh parser_vs2           # 5 minutes, one worker per core
#   bash fuzz.sh decode 3600          # an hour
#   bash fuzz.sh decode 300 2         # 5 minutes, 2 workers
#
# TARGETS
#   parser_vs2  ParserVS2::parse() -- the wire->payload state machine.
#               Saturates fast: the committed seeds already reach the coverage
#               ceiling at INITED, so a long campaign here buys little.
#   decode      the buffer-writing helpers in decode.h -- payload->text, plus
#               encode_schaltzeiten_day, the one input a PERSON types.
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
# crash file left in /tmp is a finding that will be rediscovered from scratch
# next year.
set -euo pipefail

# Resolved before anything else so the script works from any cwd; the relative
# ROOT default below is then relative to tests/native, as in build_and_run.sh.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

TARGET="${1:-}"
SECS="${2:-300}"
WORKERS="${3:-$(nproc)}"
ROOT="${VITOHOME_ROOT:-../../components/vitohome}"
OPTO="$ROOT/optolink"
FUZZ_DIR="${FUZZ_DIR:-${TMPDIR:-/tmp}/vitohome-fuzz}"

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
  *)
    echo "usage: bash fuzz.sh <target> [seconds] [workers]" >&2
    echo "targets:" >&2
    echo "  parser_vs2   ParserVS2::parse() -- wire to payload" >&2
    echo "  decode       decode.h buffer writers -- payload to text" >&2
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
mkdir -p "$CORPUS" "$FINDINGS"

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

echo "== campaign: ${SECS}s, ${WORKERS} worker(s), corpus $CORPUS =="
# Run from $WORK, not from the source tree: with -jobs, libFuzzer writes a
# fuzz-<N>.log per worker into the CURRENT directory, so launching from
# tests/native litters the checkout with untracked logs. Every path handed to
# the binary below is absolute, so the cd costs nothing.
cd "$WORK"
set +e
"$WORK/fuzz_$TARGET" "$CORPUS" \
  -artifact_prefix="$FINDINGS/" \
  -max_total_time="$SECS" \
  -workers="$WORKERS" -jobs="$WORKERS" \
  -print_final_stats=1
rc=$?
set -e

found=$(find "$FINDINGS" -type f | wc -l)
echo
if [ "$found" -gt 0 ]; then
  echo "FINDINGS: $found reproducer(s) in $FINDINGS"
  find "$FINDINGS" -type f -printf '  %p (%s bytes)\n'
  echo "Replay one against the CI build, minimise it, then promote it into $HEADER."
  exit 1
fi
echo "no findings. corpus is now $(find "$CORPUS" -type f | wc -l) units."
exit $rc
