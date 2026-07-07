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
set -euo pipefail
ROOT="${1:-../../components/vitohome}"
OPTO="$ROOT/optolink"
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
  g++ -std=c++17 -Wall -Wextra -pthread $flag -I"$ROOT" -I"$OPTO" \
    engine_compile_proof.cpp "${SRCS[@]}" -o engine_proof
  printf '  %-5s ' "$name"
  ./engine_proof
done

echo "== GWG read/write completion (THIRD_PARTY.md #8 fix) =="
g++ -std=c++17 -Wall -Wextra -pthread -DVITOHOME_PROTOCOL_GWG -I"$ROOT" -I"$OPTO" \
  proof_gwg_read.cpp "${SRCS[@]}" -o gwg_read
./gwg_read

echo "== VS1/KW write-ack completion (THIRD_PARTY.md #11 fix) =="
g++ -std=c++17 -Wall -Wextra -pthread -DVITOHOME_PROTOCOL_KW -I"$ROOT" -I"$OPTO" \
  proof_vs1_write.cpp "${SRCS[@]}" -o vs1_write
./vs1_write

echo "== VS2 guards: ERROR-type frames + parser reset (#9 / #10) =="
g++ -std=c++17 -Wall -Wextra -pthread -I"$ROOT" -I"$OPTO" \
  proof_vs2_guards.cpp "${SRCS[@]}" -o vs2_guards
./vs2_guards

echo "== GWG sync poke: must be OFF by default =="
g++ -std=c++17 -Wall -Wextra -pthread -DVITOHOME_PROTOCOL_GWG -I"$ROOT" -I"$OPTO" \
  proof_gwg_poke.cpp "${SRCS[@]}" -o gwg_poke
out="$(./gwg_poke)"
echo "  $out"
case "$out" in
  *eot_poke_emitted=0*) ;;
  *) echo "FAIL: GWG sync poke must default to OFF (no EOT)"; exit 1 ;;
esac

echo "protocol proofs OK"
