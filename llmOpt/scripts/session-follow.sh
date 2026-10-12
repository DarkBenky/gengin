#!/usr/bin/env bash
# Live view of the newest supervised session: the Hermes state.db or, for dsh
# sessions, the supervisor's tee of the agent stream. supervisor-console.sh
# opens this in a second tmux pane; run it directly to watch a session from
# anywhere (the state.db is agent-owned, mode 600, so run as root).
#
#   <checkout>/llmOpt/scripts/session-follow.sh [checkout]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec /usr/bin/python3 "$SCRIPT_DIR/session-follow.py" "$@"
