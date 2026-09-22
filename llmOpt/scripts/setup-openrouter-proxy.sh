#!/usr/bin/env bash
# Install/refresh the gengin OpenRouter filtering proxy as a systemd user unit.
#
#   llmOpt/scripts/setup-openrouter-proxy.sh
#
# Idempotent: renders llmOpt/systemd/gengin-openrouter-proxy.user.service into
# ~/.config/systemd/user/, enables it, and verifies /health.  Hermes reaches
# the proxy through model.base_url in llmOpt/.hermes/config.yaml (rendered by
# setup-hermes.sh), so this must be running for manual sessions.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLMOPT_DIR="$(dirname "$SCRIPT_DIR")"
PROXY="$LLMOPT_DIR/proxy/openrouter_proxy.py"
UNIT_SRC="$LLMOPT_DIR/systemd/gengin-openrouter-proxy.user.service"
UNIT_DIR="$HOME/.config/systemd/user"
UNIT="$UNIT_DIR/gengin-openrouter-proxy.service"
PORT="${GENGIN_PROXY_PORT:-8787}"

[[ -f "$PROXY" ]] || { echo "error: missing $PROXY" >&2; exit 1; }
[[ -f "$UNIT_SRC" ]] || { echo "error: missing $UNIT_SRC" >&2; exit 1; }

mkdir -p "$UNIT_DIR"
sed -e "s|__PROXY__|$PROXY|g" "$UNIT_SRC" > "$UNIT"

systemctl --user daemon-reload
systemctl --user enable --now gengin-openrouter-proxy.service
sleep 1

if curl -fsS "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then
  echo "proxy healthy on http://127.0.0.1:$PORT"
else
  echo "error: proxy did not become healthy — check: journalctl --user -u gengin-openrouter-proxy" >&2
  exit 1
fi

if ! loginctl show-user "$USER" 2>/dev/null | grep -q "Linger=yes"; then
  echo "note: proxy only runs while you are logged in; enable lingering:"
  echo "      sudo loginctl enable-linger $USER"
fi

echo "status: systemctl --user status gengin-openrouter-proxy"
