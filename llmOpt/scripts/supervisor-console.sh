#!/usr/bin/env bash
# Run the gengin-llmopt supervisor in a console (e.g. a tmux pane) with the
# same environment the systemd unit provides: system interpreter (never the
# agent-writable venv), secrets from the single root-only file, shared-group
# umask, and the checkout as the working directory.
#
#   tmux new-session -d -s llmopt <checkout>/llmOpt/scripts/supervisor-console.sh
#
# Inside tmux it also opens an "agent session" pane (session-follow.sh) so the
# model's turns and tool calls are visible while it works; `hermes -z` only
# prints the final answer, so this pane alone would sit silent for hours.
#
# The OpenRouter filtering proxy is managed here, not enabled at boot: if it is
# not already healthy it is started via the systemd unit when one is installed
# (started, not enabled), otherwise as a background process owned by
# llmopt-supervisor.  A proxy this script started is stopped again on exit; a
# systemd-managed one is left running.
#
# Optional run-level route overrides (they win over llmOpt/.env for this run):
#
#   supervisor-console.sh --max-context 99K --fallback-model MODEL
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

usage() {
  echo "usage: $0 [--max-context SIZE] [--fallback-model MODEL] [--budget USD]"
  echo "       Run-level cost-aware route overrides (SIZE like 99K or 1M; both"
  echo "       flags must be given together). They win over llmOpt/.env"
  echo "       GENGIN_ROUTE_MAX_CONTEXT / GENGIN_ROUTE_FALLBACK_MODEL."
  echo "       --budget caps the session spend (proxy warns, then stops at it)."
}

ROUTE_MAX_CONTEXT_ARG=""
ROUTE_FALLBACK_ARG=""
ROUTE_BUDGET_ARG=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --max-context)
      [[ $# -ge 2 ]] || { echo "error: --max-context needs a value" >&2; exit 2; }
      ROUTE_MAX_CONTEXT_ARG="$2"; shift 2 ;;
    --fallback-model)
      [[ $# -ge 2 ]] || { echo "error: --fallback-model needs a value" >&2; exit 2; }
      ROUTE_FALLBACK_ARG="$2"; shift 2 ;;
    --budget)
      [[ $# -ge 2 ]] || { echo "error: --budget needs a value" >&2; exit 2; }
      ROUTE_BUDGET_ARG="$2"; shift 2 ;;
    -h|--help)
      usage; exit 0 ;;
    *)
      echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -n "$ROUTE_MAX_CONTEXT_ARG" || -n "$ROUTE_FALLBACK_ARG" ]]; then
  [[ -n "$ROUTE_MAX_CONTEXT_ARG" && -n "$ROUTE_FALLBACK_ARG" ]] || {
    echo "error: --max-context and --fallback-model must be given together" >&2
    exit 2; }
  [[ "$ROUTE_MAX_CONTEXT_ARG" =~ ^[0-9]+[KkMm]?$ ]] || {
    echo "error: --max-context expects a token count like 99000 or 99K" >&2
    exit 2; }
  [[ "$ROUTE_FALLBACK_ARG" =~ ^[A-Za-z0-9._:/-]+$ ]] || {
    echo "error: invalid --fallback-model: $ROUTE_FALLBACK_ARG" >&2
    exit 2; }
fi

if [[ -n "$ROUTE_BUDGET_ARG" ]]; then
  [[ "$ROUTE_BUDGET_ARG" =~ ^[0-9]+([.][0-9]+)?$ ]] || {
    echo "error: --budget expects a positive USD amount" >&2; exit 2; }
fi

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

# A second pane shows the agent's session as it happens: `hermes -z` prints
# only the final answer, so this pane would otherwise sit silent for hours.
follow_pane() {
  command -v tmux >/dev/null 2>&1 || return 0
  [[ -n "${TMUX:-}" ]] || return 0
  tmux list-panes -F '#{pane_title}' 2>/dev/null | grep -qx "agent session" && return 0
  tmux split-window -v -l 35% -c "$CHECKOUT" \
    "'$SCRIPT_DIR/session-follow.sh'; echo; echo '[follow] ended - Ctrl-b x closes this pane'; exec bash" \
    2>/dev/null || return 0
  tmux select-pane -T "agent session" 2>/dev/null || true
  tmux select-pane -l 2>/dev/null || true
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
follow_pane

# Secrets are fed through stdin, never through argv or the environment of any
# world-readable process: `ps` shows command lines to every local user, and
# the sandboxed agent must not be able to scrape the management key.
route_env=()
if [[ -n "$ROUTE_MAX_CONTEXT_ARG" ]]; then
  route_env+=("GENGIN_ROUTE_MAX_CONTEXT=$ROUTE_MAX_CONTEXT_ARG")
fi
if [[ -n "$ROUTE_FALLBACK_ARG" ]]; then
  route_env+=("GENGIN_ROUTE_FALLBACK_MODEL=$ROUTE_FALLBACK_ARG")
fi
if [[ -n "$ROUTE_BUDGET_ARG" ]]; then
  route_env+=("GENGIN_ROUTE_BUDGET_USD=$ROUTE_BUDGET_ARG")
fi
if [[ ${#route_env[@]} -gt 0 ]]; then
  echo "route override: ${route_env[*]}"
fi

set +e
sudo -H -u llmopt-supervisor env "${route_env[@]}" bash -c '
  OPENROUTER_MANAGEMENT_KEY=""
  GITHUB_TOKEN=""
  DEEPSEEK_API_KEY=""
  while IFS="=" read -r name value; do
    case "$name" in
      OPENROUTER_MANAGEMENT_KEY) OPENROUTER_MANAGEMENT_KEY="$value" ;;
      GITHUB_TOKEN) GITHUB_TOKEN="$value" ;;
      DEEPSEEK_API_KEY) DEEPSEEK_API_KEY="$value" ;;
    esac
  done
  export OPENROUTER_MANAGEMENT_KEY GITHUB_TOKEN DEEPSEEK_API_KEY
  umask 0007
  exec /usr/bin/python3 llmOpt/supervisor.py run
' < "$SECRETS"
rc=$?
set -e
exit "$rc"
