#!/usr/bin/env bash
# Host build + run of the per-protocol engine proofs against all three vendored
# engines (P300 / KW / GWG). Proves OptolinkEngine<SelectedProtocol> compiles
# and links for every build-time-selected protocol flag with the uniform
# byte-mover callback shape (the hub drives the engine directly; there is no
# adapter layer), and asserts the GWG sync-poke switch
# (GWGEngine::SEND_ENQ_POKE) is OFF by default (no EOT emitted). Does not
# exercise real wire behaviour.
#
# Deliberately compiled WITHOUT the datapoint/converter translation units:
# the engine layer has no Datapoint/Converter dependency, and this script is
# the proof.
#
# The poke-ON path (EOT 0x04 emitted) is verified separately by flipping the
# switch; this script guards the default-off invariant so it can't regress.
#
# EVERY g++ line here carries -Wall -Wextra -Werror AND $SAN; see the notes in
# build_and_run.sh for why both are hard requirements and not preferences.
set -euo pipefail
ROOT="${1:-../../components/vitohome}"
OPTO="$ROOT/optolink"

# Sanitizers. Inherited from build_and_run.sh when chained (it exports SAN), so
# there is one effective value per run; the default keeps this script correct
# when it is invoked standalone, as its own README documents. Overridable --
# `SAN= bash build_and_run_protocols.sh` builds unsanitized -- which is a
# debugging convenience, NOT a supported way to pass the gate.
#
# -fno-sanitize-recover=all is the load-bearing half: without it UBSan prints a
# runtime error and CARRIES ON, so the process still exits 0 and the failure
# lands in a green build. ASan aborts by default; UBSan does not.
SAN="${SAN-"-fsanitize=address,undefined -fno-sanitize-recover=all"}"
SRCS=(
  "$OPTO/constants.cpp"
  "$OPTO/protocol/vs2/vs2.cpp"
  "$OPTO/protocol/vs2/parser_vs2.cpp"
  "$OPTO/protocol/vs2/packet_vs2.cpp"
  "$OPTO/protocol/vs1/vs1.cpp"
  "$OPTO/protocol/vs1/packet_vs1.cpp"
  "$OPTO/protocol/gwg/gwg.cpp"
  "$OPTO/protocol/gwg/packet_gwg.cpp"
)

echo "== protocol engine: compile + link for each protocol =="
for sel in "P300:" "KW:-DVITOHOME_PROTOCOL_KW" "GWG:-DVITOHOME_PROTOCOL_GWG"; do
  name="${sel%%:*}"
  flag="${sel#*:}"
  g++ -std=c++17 -Wall -Wextra -Werror $SAN -pthread $flag -I"$ROOT" -I"$OPTO" \
    engine_compile_proof.cpp "${SRCS[@]}" -o engine_proof
  printf '  %-5s ' "$name"
  ./engine_proof
done

echo "== GWG read/write completion (THIRD_PARTY.md #8 fix) =="
g++ -std=c++17 -Wall -Wextra -Werror $SAN -pthread -DVITOHOME_PROTOCOL_GWG -I"$ROOT" -I"$OPTO" \
  proof_gwg_read.cpp "${SRCS[@]}" -o gwg_read
./gwg_read

echo "== GWG dispatch-stall discriminator (isBusy() permanent vs transient) =="
g++ -std=c++17 -Wall -Wextra -Werror $SAN -pthread -DVITOHOME_PROTOCOL_GWG -I"$ROOT" -I"$OPTO" \
  proof_gwg_dispatch_stall.cpp "${SRCS[@]}" -o gwg_dispatch_stall
./gwg_dispatch_stall

echo "== GWG ENQ-misread guard: response deadline + same-loop send =="
g++ -std=c++17 -Wall -Wextra -Werror $SAN -pthread -DVITOHOME_PROTOCOL_GWG -I"$ROOT" -I"$OPTO" \
  proof_gwg_enq_misread.cpp "${SRCS[@]}" -o gwg_enq_misread
./gwg_enq_misread

echo "== GWG burst: reuse a valid sync instead of waiting for the next ENQ =="
g++ -std=c++17 -Wall -Wextra -Werror $SAN -pthread -DVITOHOME_PROTOCOL_GWG -I"$ROOT" -I"$OPTO" \
  proof_gwg_burst.cpp "${SRCS[@]}" -o gwg_burst
./gwg_burst

echo "== GWG write access modes (EEPROM/BE; KMBUS refused) =="
g++ -std=c++17 -Wall -Wextra -Werror $SAN -pthread -DVITOHOME_PROTOCOL_GWG -I"$ROOT" -I"$OPTO" \
  proof_gwg_write_access.cpp "${SRCS[@]}" -o gwg_write_access
./gwg_write_access

echo "== GWG access modes + timing instrument =="
g++ -std=c++17 -Wall -Wextra -Werror $SAN -pthread -DVITOHOME_PROTOCOL_GWG -I"$ROOT" -I"$OPTO" \
  proof_gwg_access_mode.cpp "${SRCS[@]}" -o gwg_access_mode
./gwg_access_mode

echo "== VS1/KW chaining + the two split sync windows (#22) =="
g++ -std=c++17 -Wall -Wextra -Werror $SAN -pthread -DVITOHOME_PROTOCOL_KW -I"$ROOT" -I"$OPTO" \
  proof_vs1_chain.cpp "${SRCS[@]}" -o vs1_chain
./vs1_chain

echo "== VS1/KW write-ack completion (THIRD_PARTY.md #11 fix) =="
g++ -std=c++17 -Wall -Wextra -Werror $SAN -pthread -DVITOHOME_PROTOCOL_KW -I"$ROOT" -I"$OPTO" \
  proof_vs1_write.cpp "${SRCS[@]}" -o vs1_write
./vs1_write

echo "== VS2 guards: ERROR-type frames + parser reset (#9 / #10) =="
g++ -std=c++17 -Wall -Wextra -Werror $SAN -pthread -I"$ROOT" -I"$OPTO" \
  proof_vs2_guards.cpp "${SRCS[@]}" -o vs2_guards
./vs2_guards

echo "== GWG sync poke: must be OFF by default =="
g++ -std=c++17 -Wall -Wextra -Werror $SAN -pthread -DVITOHOME_PROTOCOL_GWG -I"$ROOT" -I"$OPTO" \
  proof_gwg_poke.cpp "${SRCS[@]}" -o gwg_poke
out="$(./gwg_poke)"
echo "  $out"
case "$out" in
  *eot_poke_emitted=0*) ;;
  *) echo "FAIL: GWG sync poke must default to OFF (no EOT)"; exit 1 ;;
esac

echo "protocol proofs OK"
