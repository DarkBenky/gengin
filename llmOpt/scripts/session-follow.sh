#!/usr/bin/env bash
# Live view of the newest supervised session (tailing its Hermes state.db).
# supervisor-console.sh opens this in a second tmux pane; run it directly to
# watch a session from anywhere, as root (state.db is agent-owned, mode 600).
#
#   <checkout>/llmOpt/scripts/session-follow.sh [checkout]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec /usr/bin/python3 "$SCRIPT_DIR/session-follow.py" "$@"
