#!/usr/bin/env bash
# Run the gengin-llopt supervisor in a console (e.g. a tmux pane) with the
# same environment the systemd unit provides: system interpreter (never the
# agent-writable venv), secrets from the single root-only file, shared-group
# umask, and the checkout as the working directory.
#
#   tmux new-session -d -s llmopt /opt/gengin/llmOpt/scripts/supervisor-console.sh
#
# Rotate keys by editing /etc/gengin-llmopt/secrets.env (management key +
# GITHUB_TOKEN). The systemd unit must be stopped first: it and this console
# must not poll at the same time (systemctl disable --now gengin-llmopt.service).

set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "error: run as root (reads the credential, switches UID)" >&2; exit 1; }

SECRETS=/etc/gengin-llmopt/secrets.env
[[ -f "$SECRETS" ]] || { echo "error: missing $SECRETS" >&2; exit 1; }
OPENROUTER_MANAGEMENT_KEY="$(sed -n 's/^OPENROUTER_MANAGEMENT_KEY=//p' "$SECRETS" | head -1)"
GITHUB_TOKEN="$(sed -n 's/^GITHUB_TOKEN=//p' "$SECRETS" | head -1)"

CHECKOUT=/opt/gengin
cd "$CHECKOUT"

echo "gengin-llmopt supervisor console — $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "checkout: $CHECKOUT"
echo "(Ctrl-b d detaches; stopping this pane stops the loop)"
echo

exec sudo -H -u llmopt-supervisor env \
  OPENROUTER_MANAGEMENT_KEY="$OPENROUTER_MANAGEMENT_KEY" \
  GITHUB_TOKEN="$GITHUB_TOKEN" \
  bash -c 'umask 0007; exec /usr/bin/python3 llmOpt/supervisor.py run'
