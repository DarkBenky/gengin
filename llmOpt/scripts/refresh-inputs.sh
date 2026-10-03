#!/usr/bin/env bash
# Refresh the gengin-llmopt inputs dir + hash manifest from a checkout's
# ignored build inputs (deps/assets/.flamegraph + default.profdata).
#
# Use after changing a model/asset - the full setup-vm.sh bootstrap is not
# needed for that.  Put the new file into the checkout first (assets/models/
# is gitignored, so scp it there), then run this on the VM.
#
# Run as root:  sudo bash llmOpt/scripts/refresh-inputs.sh [options]
#
# Options:
#   --checkout DIR   path to the gengin checkout (default: the repo this script is in)
#   --inputs DIR     stable ignored-inputs dir (default: /var/lib/gengin-llmopt/inputs)

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CHECKOUT=$(cd "$SCRIPT_DIR/../.." && pwd)
INPUTS=/var/lib/gengin-llmopt/inputs

while [[ $# -gt 0 ]]; do
  case "$1" in
    --checkout) CHECKOUT="$2"; shift 2 ;;
    --inputs)   INPUTS="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

log() { echo "[refresh-inputs] $*"; }
die() { echo "[refresh-inputs] ERROR: $*" >&2; exit 1; }

[[ $EUID -eq 0 ]] || die "run as root (sudo)"
[[ -f "$CHECKOUT/llmOpt/requirements-mcp.txt" ]] || die "not a gengin checkout: $CHECKOUT"

log "refreshing inputs dir: $INPUTS"
mkdir -p "$INPUTS"
for rel in deps assets .flamegraph; do
  if [[ -e "$CHECKOUT/$rel" ]]; then
    rsync -a "$CHECKOUT/$rel/" "$INPUTS/$rel/"
  fi
done
[[ -f "$CHECKOUT/default.profdata" ]] && cp -f "$CHECKOUT/default.profdata" "$INPUTS/default.profdata"

( cd "$CHECKOUT/llmOpt" && python3 -c "import main; print(main.generate_inputs_manifest('$INPUTS'))" ) \
  || die "inputs manifest generation failed"

chown -R root:root "$INPUTS"
chmod -R a+rX "$INPUTS"
chmod 0755 "$INPUTS"
log "done; sessions pick up the new manifest on their next prepare"
