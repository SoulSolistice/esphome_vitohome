#!/usr/bin/env bash
# Host build + run of the P300 (VS2) transaction harness against the in-tree
# vendored Optolink engine (components/vitohome/optolink/).
#
# ROOT = component root that contains optolink/. The test includes the engine as
#   #include "optolink/optolink.h"
# so the component dir is on the include path; the engine's own headers use
# paths relative to their own location, which resolve from there.
#
# This first harness compiles only the P300 path (OptolinkEngine<P300> ==
# VS2Engine) for the transaction vectors; build_and_run_protocols.sh (chained at
# the end) compiles the adapter against all three engines.
set -euo pipefail
ROOT="${1:-../../components/vitohome}"
OPTO="$ROOT/optolink"
g++ -std=c++17 -Wall -Wextra \
  -I"$ROOT" -I"$OPTO" \
  test_vs2_transaction.cpp \
  "$OPTO/constants.cpp" \
  "$OPTO/protocol/vs2/vs2.cpp" \
  "$OPTO/protocol/vs2/parser_vs2.cpp" \
  "$OPTO/protocol/vs2/packet_vs2.cpp" \
  -o vs2_transaction_harness
# Engine debug logging is compiled out on host (logging.h is gated on
# VITOHOME_DEBUG_OPTOLINK && ESP_PLATFORM), so stdout is the harness only.
./vs2_transaction_harness

# Parser regression: a zero-payload VS2 frame must not walk past the packet
# buffer (inherited upstream OOB, fixed in parser_vs2.cpp). Built under
# AddressSanitizer/UBSan so the pre-fix code would trap here.
g++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined \
  -I"$ROOT" -I"$OPTO" \
  proof_vs2_zero_payload.cpp \
  "$OPTO/protocol/vs2/parser_vs2.cpp" \
  "$OPTO/protocol/vs2/packet_vs2.cpp" \
  "$OPTO/constants.cpp" \
  -o proof_vs2_zero_payload
./proof_vs2_zero_payload

# Decode proof: multi-byte field extraction from a wide block read (the
# P300-portable pattern gen_catalog emits for interior fields).
g++ -std=gnu++20 -Wall -Wextra -I"$ROOT" -I"$OPTO" proof_extract.cpp -o proof_extract
./proof_extract

# String-offset proof: ascii/utf16 fields at BytePosition > 0 must be sliced out
# of an aligned block read, never addressed at base+offset (P300 errors on the
# interior address at any width; KW returns 0xFF fill, which decodes to "").
g++ -std=gnu++20 -Wall -Wextra -fsanitize=address,undefined \
  -I"$ROOT" -I"$OPTO" proof_string_offset.cpp -o proof_string_offset
./proof_string_offset

# Scheduler proof: per-entity poll intervals must fire on every hub tick when
# interval == the hub tick, must not drift, and must survive the millis() wrap.
# (Anchoring the next due time on `now` made this a jitter-decided coin flip.)
g++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined \
  -I"$ROOT" proof_scheduler.cpp -o proof_scheduler
./proof_scheduler

# Protocol-adapter proofs: all three engines compile + the GWG poke stays off.
bash "$(dirname "$0")/build_and_run_protocols.sh" "$ROOT"
