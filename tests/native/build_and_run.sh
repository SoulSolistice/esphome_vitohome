#!/usr/bin/env bash
# Host build + run of the P300 transaction harness against the component's
# P300 exchange (components/vitohome/optolink_p300.h).
#
# ROOT = the component root; it and optolink/ are on the include path.
#
# This first harness runs the transaction vectors through the P300 exchange;
# build_and_run_protocols.sh (chained at the end) builds every protocol's
# engine.
#
# EVERY g++ line here carries -Wall -Wextra -Werror. The -Werror is not
# decoration: without it these compiles emit diagnostics and exit 0, so a new
# warning lands in a green build and is never seen. ci.yml's separate
# test_decode line has always had -Werror; these did not, which meant ~12 host
# compiles were computing warnings and discarding them. Keep it on every line
# added below -- a host compile that tolerates warnings is not a gate.
#
# The same argument applies to $SAN, and for a while it did not hold here.
# ASan/UBSan were opt-in per target: five binaries had them and roughly
# seventeen -- including EVERY engine proof in build_and_run_protocols.sh, i.e.
# all of the VS1/GWG sync, burst and access-mode work -- ran uninstrumented.
# That is the surface where garbled RX is the NORMAL case and where both known
# memory-safety defects (the zero-payload OOB write below, THIRD_PARTY.md #16's
# RESPONSE guards) actually lived, so it is the last place that should have
# been running without a memory checker. $SAN now goes on every g++ line in
# both scripts; keep it there.
#
# -fno-sanitize-recover=all matters as much as the sanitizers themselves:
# UBSan's default is to PRINT a runtime error and continue, leaving exit 0, so
# an undefined-behaviour hit would have been reported into a green build. ASan
# aborts on its own; UBSan has to be told to.
#
# Exported so the chained build_and_run_protocols.sh inherits one value rather
# than keeping a second copy that can drift. `SAN= bash build_and_run.sh`
# builds unsanitized for debugging; that does not count as passing the gate.
set -euo pipefail
export SAN="${SAN-"-fsanitize=address,undefined -fno-sanitize-recover=all"}"
# Split into words ONCE, here, rather than expanding $SAN unquoted on every g++
# line: SAN itself has to stay a plain string (it is exported to the chained
# script and overridden from the environment, and arrays cannot be exported),
# but the multi-flag value must reach g++ as separate arguments -- quoting it
# as "$SAN" would pass one bogus argument. An empty SAN yields an empty array.
read -r -a SAN_FLAGS <<<"$SAN"
ROOT="${1:-../../components/vitohome}"
OPTO="$ROOT/optolink"
g++ -std=c++17 -Wall -Wextra -Werror "${SAN_FLAGS[@]}" \
  -I"$ROOT" -I"$OPTO" \
  test_p300_transaction.cpp \
  "$OPTO/constants.cpp" \
  -o p300_transaction_harness
# Engine debug logging is compiled out on host (logging.h is gated on
# VITOHOME_DEBUG_OPTOLINK && ESP_PLATFORM), so stdout is the harness only.
./p300_transaction_harness

# Parser regression: a zero-payload P300 frame must not walk past the frame
# buffer (P300Parser, optolink_codec_p300.h). Built under AddressSanitizer and
# UBSan, so an out-of-bounds write traps.
g++ -std=c++17 -Wall -Wextra -Werror "${SAN_FLAGS[@]}" \
  -I"$ROOT" -I"$OPTO" \
  proof_vs2_zero_payload.cpp \
  "$OPTO/constants.cpp" \
  -o proof_vs2_zero_payload
./proof_vs2_zero_payload

# Remote procedure call (P300 function code 0x07): the parameter rides in the
# request, the answer is as long as its frame says, and reads/writes keep their
# guards. Through the exchange, so the wire bytes are checked as well as the parse.
g++ -std=c++17 -Wall -Wextra -Werror "${SAN_FLAGS[@]}" -I"$ROOT" -I"$OPTO" proof_p300_rpc.cpp "$OPTO/constants.cpp" -o proof_p300_rpc
./proof_p300_rpc

# Decode proof: multi-byte field extraction from a wide block read (the
# P300-portable pattern gen_catalog emits for interior fields).
g++ -std=gnu++20 -Wall -Wextra -Werror "${SAN_FLAGS[@]}" -I"$ROOT" -I"$OPTO" proof_extract.cpp -o proof_extract
./proof_extract

# String-offset proof: ascii/utf16 fields at BytePosition > 0 must be sliced out
# of an aligned block read, never addressed at base+offset (P300 errors on the
# interior address at any width; KW returns 0xFF fill, which decodes to "").
g++ -std=gnu++20 -Wall -Wextra -Werror "${SAN_FLAGS[@]}" \
  -I"$ROOT" -I"$OPTO" proof_string_offset.cpp -o proof_string_offset
./proof_string_offset

# Scheduler proof: per-entity poll intervals must fire on every hub tick when
# interval == the hub tick, must not drift, and must survive the millis() wrap.
# (Anchoring the next due time on `now` made this a jitter-decided coin flip.)
g++ -std=c++17 -Wall -Wextra -Werror "${SAN_FLAGS[@]}" \
  -I"$ROOT" proof_scheduler.cpp -o proof_scheduler
./proof_scheduler

# Ring-buffer proof: the fixed-capacity, task-synchronized ring that replaced
# the three std::deque run-loop queues (read/write/raw). One-shot reserve() +
# FIFO + push_front + the try_front/drop_front hand-off + wraparound + full-drop must hold, and
# a bad modulo would be an out-of-bounds slot access under ASan.
# -DVITOHOME_NATIVE_TEST selects the header's no-op host mutex stand-in
# (device builds use the real esphome::Mutex; ESPHome headers are not on the
# host include path).
g++ -std=c++17 -Wall -Wextra -Werror "${SAN_FLAGS[@]}" \
  -DVITOHOME_NATIVE_TEST \
  -I"$ROOT" proof_ring_buffer.cpp -o proof_ring_buffer
./proof_ring_buffer

# P300 parser fuzz corpus, replayed deterministically. Same source file the
# libFuzzer target is built from (fuzz.sh), minus the fuzzer driver: here it
# walks the committed seeds in fuzz_corpus_vs2.h, so the oracles CI enforces
# and the oracles a campaign enforces cannot drift apart. Sub-second, no clang
# needed. The open-ended search is deliberately not run here -- see fuzz.sh.
g++ -std=c++17 -Wall -Wextra -Werror "${SAN_FLAGS[@]}" \
  -I"$ROOT" -I"$OPTO" \
  fuzz_parser_vs2.cpp \
  "$OPTO/constants.cpp" \
  -o fuzz_parser_vs2
./fuzz_parser_vs2

# raw_op.h: the generic raw operation's spec parser and safety class
# (queue_raw_op()), then its fuzz corpus -- same two-build arrangement as the
# VS2 parser's (fuzz.sh raw_spec for the open-ended search).
g++ -std=c++17 -Wall -Wextra -Werror "${SAN_FLAGS[@]}"   -I"$ROOT" -I"$OPTO" proof_raw_op.cpp -o proof_raw_op
./proof_raw_op
g++ -std=c++17 -Wall -Wextra -Werror "${SAN_FLAGS[@]}"   -I"$ROOT" -I"$OPTO" fuzz_raw_spec.cpp -o fuzz_raw_spec
./fuzz_raw_spec
# raw_script.h: the runner of a timed open frame, on a scripted port and the
# frozen host clock.
g++ -std=c++17 -Wall -Wextra -Werror -Wformat=2 "${SAN_FLAGS[@]}" -I"$ROOT" -I"$OPTO" proof_raw_script.cpp -o proof_raw_script
./proof_raw_script

# decode.h fuzz corpus, the same two-build arrangement over the other half of
# the path: payload->text, where the fixed char buffers and the hand-rolled
# snprintf offset arithmetic live, plus encode_schaltzeiten_day, which parses a
# string a PERSON typed into Home Assistant. decode.h is header-only and
# includes itself relatively (as test_decode.cpp does), so this needs no -I and
# no extra translation units.
g++ -std=c++17 -Wall -Wextra -Werror "${SAN_FLAGS[@]}" \
  fuzz_decode.cpp -o fuzz_decode
./fuzz_decode

# decode.h fuzz corpus over the value half: field extraction, scaling and the
# encode inverses (fuzz.sh field).
g++ -std=c++17 -Wall -Wextra -Werror "${SAN_FLAGS[@]}" \
  fuzz_field.cpp -o fuzz_field
./fuzz_field

# Protocol-adapter proofs: all three engines compile + the GWG poke stays off.
bash "$(dirname "$0")/build_and_run_protocols.sh" "$ROOT"
