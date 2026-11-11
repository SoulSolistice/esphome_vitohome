#!/usr/bin/env bash
# Validate generated catalogs against the component's config schema.
#
# example/catalogs/ is generator output (scripts/gen_catalog.py), and the
# generator's unit tests check it against fixtures -- not against the schema
# in components/vitohome/. validate_examples.sh skips the directory, and no
# test config includes a generated catalog, so a catalog the component
# rejects would be published unnoticed. This closes that gap: each catalog is
# included as a package into a minimal device config, exactly as a user
# includes it, and passed through `esphome config`.
#
# Usage, from anywhere:
#   bash scripts/validate_catalogs.sh              the sample below (CI)
#   bash scripts/validate_catalogs.sh --all        every catalog, both languages
#   bash scripts/validate_catalogs.sh de/x.yaml    the named ones, relative to
#                                                  example/catalogs/
#
# A `+` joins packages validated together: a GFA companion extends entities of
# the catalog it sits next to and means nothing alone.
#
# The hub protocol is GWG for gwg_*.yaml and P300 for everything else. The
# catalogs are written for P300 (`access: rpc` exists nowhere else), which is
# also what a KW-capable unit is normally run on.
set -euo pipefail

cd "$(dirname "$0")/.."
CAT="example/catalogs"

# One per protocol and code path, plus the largest:
#   vscotho1_72         the reference unit (P300), both languages, and with its
#                       GFA companion
#   gwg_vbes_03         GWG, the catalog issue #35's unit runs
#   gwg_vbem_36         GWG, newest revision of the other panel family
#   gwg_bt2             GWG, the second ident (0x2054)
#   cu401b_s            the largest catalog (heat pump)
#   v200kw2             a KW-era unit with remote procedure readings
#   vitocom100_lan      one of the smallest
SAMPLE=(
  "de/vscotho1_72.yaml"
  "en/vscotho1_72.yaml"
  "de/vscotho1_72.yaml+de/gfa/chip_24.yaml"
  "de/gwg_vbes_03.yaml"
  "en/gwg_vbem_36.yaml"
  "de/gwg_bt2.yaml"
  "de/cu401b_s.yaml"
  "de/v200kw2.yaml"
  "de/vitocom100_lan.yaml"
)

targets=()
if [ "$#" -eq 0 ]; then
  targets=("${SAMPLE[@]}")
elif [ "$1" = "--all" ]; then
  for f in "$CAT"/de/*.yaml "$CAT"/en/*.yaml; do
    targets+=("${f#"$CAT"/}")
  done
else
  targets=("$@")
fi

# Native Windows path under Git Bash (esphome.exe does not read /e/...), the
# plain one everywhere else. Same idiom as validate_user_curated.sh.
root="$(pwd -W 2>/dev/null || pwd)"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
# Same resolution for the scratch directory.
work_native="$(cd "$work" && (pwd -W 2>/dev/null || pwd))"

checked=0
failures=0

for target in "${targets[@]}"; do
  first="${target%%+*}"
  case "$(basename "$first")" in
    gwg_*) protocol="GWG" ;;
    *) protocol="P300" ;;
  esac

  packages=""
  missing=""
  n=0
  IFS='+' read -r -a parts <<<"$target"
  for part in "${parts[@]}"; do
    if [ ! -f "$CAT/$part" ]; then
      missing="$part"
      break
    fi
    packages+="  p${n}: !include ${root}/${CAT}/${part}"$'\n'
    n=$((n + 1))
  done

  checked=$((checked + 1))
  if [ -n "$missing" ]; then
    echo "FAIL  $target"
    echo "    $CAT/$missing does not exist"
    failures=$((failures + 1))
    continue
  fi

  cfg="$work/check.yaml"
  cat >"$cfg" <<EOF
esphome:
  name: catalog-check

esp32:
  board: esp32dev
  framework:
    type: esp-idf

external_components:
  - source:
      type: local
      path: ${root}/components
    components: [vitohome]

logger:

uart:
  - id: uart_optolink
    rx_pin: GPIO16
    tx_pin: GPIO17
    baud_rate: 4800
    data_bits: 8
    parity: EVEN
    stop_bits: 2

vitohome:
  id: vito
  uart_id: uart_optolink
  protocol: ${protocol}

packages:
${packages}
EOF

  if out="$(esphome config "$work_native/check.yaml" 2>&1)"; then
    echo "PASS  $target  ($protocol)"
  else
    echo "FAIL  $target  ($protocol)"
    # The dump of a failed config ends with the errors; the head is the
    # config itself, thousands of lines for a large catalog.
    printf '%s\n' "$out" | tail -n 40 | sed 's/^/    /'
    failures=$((failures + 1))
  fi
done

echo "---"
echo "catalogs validated: $checked   failures: $failures"

if [ "$checked" -eq 0 ]; then
  echo "ERROR: nothing was validated -- the target list is empty, which means the"
  echo "       glob or the sample broke rather than that the catalogs vanished."
  exit 1
fi

[ "$failures" -eq 0 ]
