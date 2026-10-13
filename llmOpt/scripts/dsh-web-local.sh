#!/usr/bin/env bash
# Launch the DeepSeek Harness (dsh) Web UI against the local model server, for
# interactive browser chat only - no MCP tools, no optimizer wiring.
#
#   llmOpt/scripts/dsh-web-local.sh [model-id]
#
# Serves the Web UI at http://127.0.0.1:3080 (dsh default). The model comes from
# the local OpenAI-compatible endpoint (default http://localhost:8013/v1, the
# llmOpt router: fast 7B by default, Strata/27B in big mode). Override the
# endpoint with LOCAL_LLM_BASE, the model id with the first argument (or it is
# discovered from /v1/models). Runs as the dsh-web-local systemd user unit
# (llmOpt/systemd/) for login autostart; DSH_WEB_WAIT_SECONDS then makes
# startup wait for the router instead of serving a bogus model id.
#
# DSH_HOME is scoped to llmOpt/.dsh so the global ~/.dsh is never touched. The
# web profile patch is rendered fresh on every run. Proprietary-code opt-out:
# the DeepSeek session-log row is disabled and OTel export is switched off.
# The login token is pinned (llmOpt/.dsh/web-launch-token) so the bookmark URL
# and the browser cookie keep working across service restarts.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLMOPT_DIR="$(dirname "$SCRIPT_DIR")"
DSH_DIR="$LLMOPT_DIR/.dsh"
PATCH_DIR="$DSH_DIR/profiles/web"
PATCH_FILE="$PATCH_DIR/cordis.patch.yml"

BASE_URL="${LOCAL_LLM_BASE:-http://localhost:8013/v1}"
PORT="${DSH_WEB_PORT:-3080}"

if ! command -v dsh >/dev/null 2>&1; then
  echo "error: dsh not installed" >&2
  echo "  install: npm install -g @deepseek-ai/dsh   # Node.js 18+" >&2
  exit 127
fi

# Model id: argument wins, else discover from the endpoint. The router answers
# /v1/models in OpenAI shape when the big model is loaded and in llama.cpp
# native shape otherwise, so both are parsed; the id is display-only (the
# router ignores it when forwarding). DSH_WEB_WAIT_SECONDS (set by the systemd
# unit) waits for the endpoint to come up - login autostart can outrun the
# router - and fails hard after the deadline so the unit restarts.
discover_model() {
  curl -fsS --max-time 5 "$BASE_URL/models" 2>/dev/null | python3 -c '
import sys, json
d = json.load(sys.stdin)
for m in (d.get("data") or []) + (d.get("models") or []):
    if isinstance(m, dict) and (m.get("id") or m.get("name") or m.get("model")):
        print(m.get("id") or m.get("name") or m.get("model"))
        break
' 2>/dev/null || true
}
endpoint_ready() { curl -fsS --max-time 5 -o /dev/null "$BASE_URL/models" 2>/dev/null; }

MODEL_ID="${1:-}"
[[ -n "$MODEL_ID" ]] || MODEL_ID="$(discover_model)"
WAIT_SECONDS="${DSH_WEB_WAIT_SECONDS:-0}"
if [[ -z "$MODEL_ID" && "$WAIT_SECONDS" -gt 0 ]]; then
  deadline=$((SECONDS + WAIT_SECONDS))
  until endpoint_ready; do
    if [[ $SECONDS -ge $deadline ]]; then
      echo "error: $BASE_URL/models not reachable after ${WAIT_SECONDS}s" >&2
      exit 1
    fi
    sleep 2
  done
  MODEL_ID="$(discover_model)"
fi
if [[ -z "$MODEL_ID" ]]; then
  echo "warning: could not discover a model from $BASE_URL/models - is the server up?" >&2
  MODEL_ID="local-model"
fi

# pi-ai's openai-completions path still requires a key/Authorization even for a
# keyless local server, so reference a placeholder credential (never a secret).
export LOCAL_LLM_KEY="${LOCAL_LLM_KEY:-none}"

mkdir -p "$PATCH_DIR"
cat > "$PATCH_FILE" <<YAML
# Rendered by llmOpt/scripts/dsh-web-local.sh - interactive browser chat on the
# local model only. Regenerated every run; edit the script, not this file.
- id: llm-pi-ai
  config:
    providers:
      local-gengin:
        apiKeyEnv: LOCAL_LLM_KEY
        api: openai-completions
        baseURL: $BASE_URL
        compat:
          supportsDeveloperRole: false
          maxTokensField: max_tokens
        models:
          - id: $MODEL_ID
- id: agent-default-model
  config:
    provider: local-gengin
    model: $MODEL_ID
# Proprietary code: no session-log contribution to DeepSeek.
- id: session-log-deepseek
  disabled: true
# Model-only UI: no web search/fetch tools (they target the DeepSeek API).
- id: tool-web
  disabled: true
YAML
chmod 600 "$PATCH_FILE"

# The web UI mints a random launch token per process, so every restart breaks
# bookmarks and forces re-authentication from the journal. The shared helper
# pins one token (llmOpt/.dsh/web-launch-token) and makes the installed
# dsh-client-connection bundle read it; re-applied on every run, so a dsh
# update self-heals. gengin-opt.sh local uses the same helper for the
# optimizer Web UI.
source "$SCRIPT_DIR/dsh-web-common.sh"
dshWebPrepareLaunch "$DSH_DIR"

# The OTel row is launcher-owned (config cannot disable it): opt out via env.
export DSH_TELEMETRY_MODE=DISABLED DSH_TELEMETRY_DISABLED=1

echo "dsh web: model '$MODEL_ID' via $BASE_URL"
echo "DSH_HOME: $DSH_DIR   patch: $PATCH_FILE"
echo "open http://127.0.0.1:$PORT  (Ctrl+C to stop)"
echo "bookmark (stable login token): http://127.0.0.1:$PORT/?token=$DSH_LAUNCH_TOKEN"
echo

DSH_HOME="$DSH_DIR" exec dsh web --port "$PORT" --no-open
