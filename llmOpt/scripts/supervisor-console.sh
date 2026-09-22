#!/usr/bin/env bash
# Run the gengin-llmopt supervisor in a console (e.g. a tmux pane) with the
# same environment the systemd unit provides: system interpreter (never the
# agent-writable venv), secrets from the single root-only file, shared-group
# umask, and the checkout as the working directory.
#
#   tmux new-session -d -s llmopt <checkout>/llmOpt/scripts/supervisor-console.sh
#
# The OpenRouter filtering proxy is managed here, not enabled at boot: if it is
# not already healthy it is started via the systemd unit when one is installed
# (started, not enabled), otherwise as a background process owned by
# llmopt-supervisor.  A proxy this script started is stopped again on exit; a
# systemd-managed one is left running.
#
# Rotate keys by editing /etc/gengin-llmopt/secrets.env (management key +
# GITHUB_TOKEN). The systemd unit must be stopped first: it and this console
# must not poll at the same time (systemctl disable --now gengin-llmopt.service).

set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "error: run as root (reads the credential, switches UID)" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHECKOUT="$(dirname "$(dirname "$SCRIPT_DIR")")"
SECRETS=/etc/gengin-llmopt/secrets.env
PROXY_PORT="${GENGIN_PROXY_PORT:-8787}"

[[ -f "$SECRETS" ]] || { echo "error: missing $SECRETS" >&2; exit 1; }
cd "$CHECKOUT"

proxy_pid=""
cleanup() {
  if [[ -n "$proxy_pid" ]] && kill -0 "$proxy_pid" 2>/dev/null; then
    kill "$proxy_pid" 2>/dev/null || true
    wait "$proxy_pid" 2>/dev/null || true
    echo "stopped proxy (pid $proxy_pid)"
  fi
}
trap cleanup EXIT

proxy_healthy() {
  curl -fsS --max-time 2 "http://127.0.0.1:$PROXY_PORT/health" >/dev/null 2>&1
}

ensure_proxy() {
  proxy_healthy && return 0
  if systemctl list-unit-files gengin-openrouter-proxy.service >/dev/null 2>&1; then
    systemctl start gengin-openrouter-proxy.service 2>/dev/null || true
    if proxy_healthy; then
      echo "started gengin-openrouter-proxy.service (systemd, left running)"
      return 0
    fi
  fi
  echo "starting proxy as a background process (log: llmOpt/logs/proxy.log)"
  mkdir -p "$CHECKOUT/llmOpt/logs"
  chown llmopt-supervisor:gengin-llmopt "$CHECKOUT/llmOpt/logs" 2>/dev/null || true
  sudo -H -u llmopt-supervisor bash -c \
    "nohup /usr/bin/python3 '$CHECKOUT/llmOpt/proxy/openrouter_proxy.py' >> '$CHECKOUT/llmOpt/logs/proxy.log' 2>&1 & echo \$!" \
    > "$CHECKOUT/llmOpt/logs/proxy.pid"
  proxy_pid="$(cat "$CHECKOUT/llmOpt/logs/proxy.pid" 2>/dev/null || true)"
  chown llmopt-supervisor:gengin-llmopt "$CHECKOUT/llmOpt/logs/proxy.pid" \
    "$CHECKOUT/llmOpt/logs/proxy.log" 2>/dev/null || true
  for _ in $(seq 1 40); do
    proxy_healthy && { echo "proxy healthy on :$PROXY_PORT (pid $proxy_pid)"; return 0; }
    sleep 0.25
  done
  echo "error: proxy did not become healthy on :$PROXY_PORT" >&2
  return 1
}

echo "gengin-llmopt supervisor console — $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "checkout: $CHECKOUT"
echo "(Ctrl-b d detaches; stopping this pane stops the loop)"
echo

ensure_proxy || exit 1
echo

# Secrets are fed through stdin, never through argv or the environment of any
# world-readable process: `ps` shows command lines to every local user, and
# the sandboxed agent must not be able to scrape the management key.
set +e
sudo -H -u llmopt-supervisor bash -c '
  OPENROUTER_MANAGEMENT_KEY=""
  GITHUB_TOKEN=""
  while IFS="=" read -r name value; do
    case "$name" in
      OPENROUTER_MANAGEMENT_KEY) OPENROUTER_MANAGEMENT_KEY="$value" ;;
      GITHUB_TOKEN) GITHUB_TOKEN="$value" ;;
    esac
  done
  export OPENROUTER_MANAGEMENT_KEY GITHUB_TOKEN
  umask 0007
  exec /usr/bin/python3 llmOpt/supervisor.py run
' < "$SECRETS"
rc=$?
set -e
exit "$rc"
