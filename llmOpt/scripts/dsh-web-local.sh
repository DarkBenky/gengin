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
# discovered from /v1/models).
#
# DSH_HOME is scoped to llmOpt/.dsh so the global ~/.dsh is never touched. The
# web profile patch is rendered fresh on every run. Proprietary-code opt-out:
# the DeepSeek session-log row is disabled and OTel export is switched off.
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

# Model id: argument wins, else discover from the endpoint, else a sane default.
MODEL_ID="${1:-}"
if [[ -z "$MODEL_ID" ]]; then
  MODEL_ID="$(curl -fsS --max-time 5 "$BASE_URL/models" 2>/dev/null \
    | python3 -c 'import sys,json; print(json.load(sys.stdin)["data"][0]["id"])' 2>/dev/null || true)"
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

# The OTel row is launcher-owned (config cannot disable it): opt out via env.
export DSH_TELEMETRY_MODE=DISABLED DSH_TELEMETRY_DISABLED=1

echo "dsh web: model '$MODEL_ID' via $BASE_URL"
echo "DSH_HOME: $DSH_DIR   patch: $PATCH_FILE"
echo "open http://127.0.0.1:$PORT  (Ctrl+C to stop)"
echo

DSH_HOME="$DSH_DIR" exec dsh web --port "$PORT" --no-open
