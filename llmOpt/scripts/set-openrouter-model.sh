#!/usr/bin/env bash
# Change the model the supervisor runs sessions with.
#
#   sudo llmOpt/scripts/set-openrouter-model.sh provider/model[:variant]
#
# The supervisor reads OPENROUTER_MODEL from llmOpt/.env — there is no CLI flag
# and supervisor-console.sh has none either, so this rewrites that line (with a
# timestamped backup), keeps the supervisor-owned ownership/mode, and restarts
# the systemd unit when it is running.  Credentials stay in
# /etc/gengin-llmopt/secrets.env and are untouched.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLMOPT_DIR="$(dirname "$SCRIPT_DIR")"
ENV_FILE="$LLMOPT_DIR/.env"

usage() { echo "usage: $0 <provider/model[:variant]>   e.g. z-ai/glm-5.3-flash:floor" >&2; }

[[ $# -eq 1 ]] || { usage; exit 2; }
MODEL="$1"
[[ "$MODEL" =~ ^[A-Za-z0-9._:/-]+$ ]] || { echo "error: invalid model id: $MODEL" >&2; exit 2; }
[[ -f "$ENV_FILE" ]] || { echo "error: missing $ENV_FILE" >&2; exit 1; }

CURRENT="$(grep -E '^OPENROUTER_MODEL=' "$ENV_FILE" | head -1 | cut -d= -f2- || true)"
if [[ "$CURRENT" == "$MODEL" ]]; then
  echo "OPENROUTER_MODEL already $MODEL"
else
  cp -a "$ENV_FILE" "$ENV_FILE.bak-$(date -u +%Y%m%dT%H%M%SZ)"
  if grep -qE '^OPENROUTER_MODEL=' "$ENV_FILE"; then
    sed -i "s|^OPENROUTER_MODEL=.*|OPENROUTER_MODEL=$MODEL|" "$ENV_FILE"
  else
    printf 'OPENROUTER_MODEL=%s\n' "$MODEL" >> "$ENV_FILE"
  fi
  echo "OPENROUTER_MODEL: ${CURRENT:-<unset>} -> $MODEL"
fi

# VM installs keep .env owned by the supervisor group so the agent cannot read it.
if id llmopt-supervisor >/dev/null 2>&1; then
  chown llmopt-supervisor:gengin-llmopt "$ENV_FILE" 2>/dev/null || true
  chmod 0640 "$ENV_FILE" 2>/dev/null || true
fi

if systemctl is-active gengin-llmopt.service >/dev/null 2>&1; then
  systemctl restart gengin-llmopt.service
  echo "restarted gengin-llmopt.service (an in-flight session is terminated)"
elif systemctl list-unit-files gengin-llmopt.service >/dev/null 2>&1; then
  echo "gengin-llmopt.service is not running; its next start uses the new model"
else
  echo "no gengin-llmopt.service here — for the tmux console:"
  echo "  Ctrl-C in the pane, then rerun llmOpt/scripts/supervisor-console.sh"
fi
