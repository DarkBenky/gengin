#!/usr/bin/env bash
# Launch a gengin optimization session under an agent harness (dsh default,
# Hermes with --hermes / --harness hermes or GENGIN_HARNESS=hermes).
#
#   gengin-opt.sh                       default model from the harness config
#   gengin-opt.sh openrouter [model]    force OpenRouter (optionally override model)
#   gengin-opt.sh local [model]         local llama.cpp server on :8013; boots the
#                                       dsh TUI (interactive terminal UI)
#   gengin-opt.sh local --web           dsh Web UI instead (no browser popup; :3081)
#   gengin-opt.sh local --headless      auto-run one-shot instead of a UI
#   gengin-opt.sh deepseek [model]      direct DeepSeek API (needs DEEPSEEK_API_KEY)
#   gengin-opt.sh openai [model]        direct OpenAI API (needs OPENAI_API_KEY)
#   gengin-opt.sh openrouter PRIMARY --max-context 99K FALLBACK
#                                       switch to FALLBACK at 99K prompt tokens
#   gengin-opt.sh ml [local|openrouter|deepseek] [model]  ML layer objective
#   gengin-opt.sh --goal "speed up X"   append a session goal to the prompt
#   gengin-opt.sh --headless            scripted oneshot (-z), no approvals, usage report
#   gengin-opt.sh --harness hermes      run on Hermes Agent instead of dsh
#   gengin-opt.sh local --hermes        shorthand for --harness hermes (--dsh for dsh)
#   gengin-opt.sh --dry-run             print the resolved command and exit
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLMOPT_DIR="$(dirname "$SCRIPT_DIR")"
HERMES_DIR="$LLMOPT_DIR/.hermes"
DSH_DIR="$LLMOPT_DIR/.dsh"
PROMPT_FILE="$LLMOPT_DIR/prompts/optimize.md"

PROVIDER_ARGS=()
MODEL_ARGS=()
PRESET=""
HEADLESS=0
WEB_MODE=0
WEB_REQ=0
TUI_REQ=0
DRY_RUN=0
GOAL=""
ML_MODE=0
SUPERVISED=0
MODEL=""
QUERY_FILE_ARG=""
USAGE_FILE_ARG=""
ROUTE_MAX_CONTEXT=""
ROUTE_FALLBACK=""
ROUTE_BUDGET=""
WAIT_OFFPEAK=0
HARNESS="${GENGIN_HARNESS:-dsh}"

usage() {
  echo "usage: $0 [local|openrouter|deepseek|openai] [model] [--goal TEXT] [--headless] [--dry-run]"
  echo "       $0 ml [local|openrouter|deepseek|openai] [model] [--goal TEXT] [--headless]"
  echo "       $0 openrouter PRIMARY --max-context 99K FALLBACK [--goal TEXT]"
  echo "       $0 openrouter PRIMARY --max-context 99K --budget 15 deepseek"
  echo "       $0 openrouter PRIMARY --max-context 99K --budget 15 openai"
  echo "       $0 openrouter --supervised --model M --query-file F --usage-file U"
  echo "       --budget USD session spend cap; --wait-offpeak waits for half-price hours"
  echo "       --harness dsh|hermes selects the agent harness (default: dsh)"
  echo "       --hermes / --dsh are shorthand for --harness hermes|dsh"
  echo "       local boots the dsh TUI (interactive terminal UI); --web boots the"
  echo "       dsh Web UI instead (:3081, GENGIN_WEB_PORT overrides); --headless"
  echo "       keeps the auto-run one-shot; --hermes runs the Hermes TUI instead"
}

parse_tokens() {
  # 99K / 99000 / 1M -> integer tokens; nonzero exit when malformed.
  local value="$1" size unit
  [[ "$value" =~ ^([0-9]+)([KkMm]?)$ ]] || return 1
  size="${BASH_REMATCH[1]}"
  unit="${BASH_REMATCH[2]}"
  case "$unit" in
    K|k) size=$((size * 1000)) ;;
    M|m) size=$((size * 1000000)) ;;
  esac
  (( size >= 1000 && size <= 10000000 )) || return 1
  printf '%s' "$size"
}

ensure_route_proxy() {
  # Health-check the proxy; start the desktop user unit when one is installed.
  local port="$1"
  curl -fsS --max-time 2 "http://127.0.0.1:$port/health" >/dev/null 2>&1 && return 0
  if systemctl --user cat gengin-openrouter-proxy.service >/dev/null 2>&1; then
    systemctl --user start gengin-openrouter-proxy.service 2>/dev/null || true
    local _
    for _ in $(seq 1 20); do
      curl -fsS --max-time 2 "http://127.0.0.1:$port/health" >/dev/null 2>&1 && return 0
      sleep 0.25
    done
  fi
  return 1
}

ensure_tty_stdout() {
  # Interactive UIs (dsh TUI, hermes chat) need a real terminal on stdout.
  # Shell capture hooks (e.g. a zsh preexec doing `exec > >(tee log)`) pipe
  # the stdout of every command; the frontends then see a non-interactive
  # host and silently skip. Re-attach to the controlling terminal.
  if [[ -t 1 ]]; then
    return 0
  fi
  if [[ -w /dev/tty ]]; then
    exec >/dev/tty
  else
    echo "note: stdout is not a terminal - the interactive UI may not start" >&2
  fi
}

discover_local_model() {
  # The router answers /v1/models in OpenAI shape when the big model is loaded
  # and in llama.cpp native shape otherwise; the id is display-only (the router
  # ignores it when forwarding).
  curl -fsS --max-time 5 "${LOCAL_LLM_BASE:-http://localhost:8013/v1}/models" 2>/dev/null | python3 -c '
import sys, json
d = json.load(sys.stdin)
for m in (d.get("data") or []) + (d.get("models") or []):
    if isinstance(m, dict) and (m.get("id") or m.get("name") or m.get("model")):
        print(m.get("id") or m.get("name") or m.get("model"))
        break
' 2>/dev/null || true
}

local_patch_rows() {
  # The patch body shared by every local surface (headless one-shot, Web UI,
  # TUI overlay): the local router provider, the default model, the
  # session-log opt-out, the longer bash deadline and the gengin MCP wiring.
  # Args: base_url model python_bin display_value.
  local base="$1" model="$2" python_bin="$3" display_value="$4"
  cat <<YAML
- id: llm-pi-ai
  config:
    providers:
      local-gengin:
        apiKeyEnv: LOCAL_LLM_KEY
        api: openai-completions
        baseURL: $base
        compat:
          supportsDeveloperRole: false
          maxTokensField: max_tokens
        models:
          - id: $model
- id: agent-default-model
  config:
    provider: local-gengin
    model: $model
# Proprietary code: no session-log contribution to DeepSeek.
- id: session-log-deepseek
  disabled: true
# The bash tool's 60 s foreground deadline kills ordinary builds and bench
# runs; the optimizer workflow needs the long-command budget Hermes had.
- id: bash-sandbox
  config:
    timeoutMs: 1800000
# The gengin optimizer tools (same wiring as the headless profile).
- insert:
    - id: mcp-gengin
      name: '@deepseek-ai/dsh-mcp-client'
      config:
        serverName: gengin
        transport: stdio
        # Build/bench tools run for many minutes (make_bench is 5 runs); the
        # 60 s default aborts them mid-flight.
        toolCallTimeoutMs: 1800000
        command: ${python_bin:-python3}
        args: ["$LLMOPT_DIR/mcp_server.py"]
        env:
          PYTHONUNBUFFERED: "1"
          DISPLAY: !!js process.env.DISPLAY ?? '$display_value'
          OPENROUTER_API_KEY: !!js "process.env.OPENROUTER_API_KEY ?? ''"
          GITHUB_TOKEN: !!js "process.env.GITHUB_TOKEN ?? ''"
YAML
}

render_local_dsh_profile() {
  # The `local` preset renders its own dsh profiles: `local` (the headless
  # one-shot runner) and `local-web` (the interactive Web UI). The scaffolding
  # makes the profile name resolvable (non-shipped names need a manifest; the
  # bundles resolve from the dsh installation); the patch is regenerated on
  # every run so the model id follows the router state. Same MCP wiring as the
  # headless profile.
  local home="$1" profile="${2:-local}" base="${LOCAL_LLM_BASE:-http://localhost:8013/v1}"
  local model python_bin display_value patch_dir app_bundle
  case "$profile" in
    local)     app_bundle='"@deepseek-ai/dsh-headless"' ;;
    local-web) app_bundle='"@deepseek-ai/dsh-web-app"' ;;
    *) echo "error: unknown local dsh profile: $profile" >&2; return 1 ;;
  esac
  model="$(discover_local_model)"
  if [[ -z "$model" ]]; then
    echo "warning: no model from $base/models - is the local router up?" >&2
    model="local-model"
  fi
  # Web sessions persist a per-session model selection that outlives patch
  # re-renders; the router's reported id flips with its backend mode, so pin a
  # stable provider id for the interactive Web UI.
  [[ "$profile" == "local-web" ]] && model="local-router"
  python_bin="$(command -v python3 || true)"
  display_value="${GENGIN_DISPLAY:-:2}"
  patch_dir="$home/profiles/$profile"
  mkdir -p "$patch_dir"
  if [[ ! -f "$patch_dir/package.json" ]]; then
    cat > "$patch_dir/package.json" <<JSON
{
  "name": "dsh-profile-$profile",
  "private": true,
  "dependencies": {},
  "dsh": {
    "profile": {
      "bundles": [
        "@deepseek-ai/dsh-base",
        $app_bundle
      ]
    }
  }
}
JSON
    cat > "$patch_dir/cordis.yml" <<'YAML'
# dsh profile root - an empty entry list. The tree is composed as patches:
# each bundle in package.json's dsh.profile.bundles, then cordis.patch.yml.
[]
YAML
    cat > "$patch_dir/pnpm-workspace.yaml" <<'YAML'
packages:
  - .

nodeLinker: hoisted
autoInstallPeers: false
YAML
  fi
  {
    echo "# Rendered by llmOpt/scripts/gengin-opt.sh (local preset: $profile) - the"
    echo "# local OpenAI-compatible router instead of the OpenRouter proxy. Regenerated"
    echo "# on every local run; edit the script, not this file."
    local_patch_rows "$base" "$model" "$python_bin" "$display_value"
  } > "$patch_dir/cordis.patch.yml"
  chmod 600 "$patch_dir/cordis.patch.yml"
}

render_local_tui_overlay() {
  # The dsh-tui profile is owned by the TUI plugin (its settings live in
  # ~/.dsh-tui/settings.json and the profile dir), so the optimizer wiring is
  # applied as a launcher --patch overlay on top, regenerated on every run.
  # Interactive sessions persist a model selection, so the provider id is the
  # stable local-router (the router ignores ids; its reported id flips with
  # backend mode). GENGIN_TUI_MODEL overrides it for endpoints that validate
  # model names (e.g. the VM's OpenRouter proxy behind LOCAL_LLM_BASE).
  local home="$1" base="${LOCAL_LLM_BASE:-http://localhost:8013/v1}"
  local model python_bin display_value overlay
  if [[ -z "$(discover_local_model)" ]]; then
    echo "warning: no model from $base/models - is the local router up?" >&2
  fi
  model="${GENGIN_TUI_MODEL:-local-router}"
  python_bin="$(command -v python3 || true)"
  display_value="${GENGIN_DISPLAY:-:2}"
  overlay="$home/cache/gengin-tui-overlay.yml"
  mkdir -p "$home/cache"
  {
    echo "# Rendered by llmOpt/scripts/gengin-opt.sh - optimizer overlay for the"
    echo "# dsh-tui profile (--patch). Regenerated on every local run; edit the"
    echo "# script, not this file."
    local_patch_rows "$base" "$model" "$python_bin" "$display_value"
  } > "$overlay"
  chmod 600 "$overlay"
  TUI_OVERLAY="$overlay"
}

launch_supervised_tui() {
  # Run a supervised session inside a real dsh-tui in a tmux session so a
  # human can attach and watch it live (GENGIN_SUPERVISED_TUI=1). The session
  # home already carries the rendered patch layer with the proxy route, the
  # MCP wiring and the session variables (GENGIN_SESSION_RESULT_PATH), so the
  # same completion artifact the headless run writes drives this wrapper.
  # Returns when the agent reports its result; non-zero if the TUI dies first.
  # Args: session DSH_HOME.
  local home="$1"
  local result_file="${GENGIN_SESSION_RESULT_PATH:-}"
  local run_tmux="gengin-run"
  local patch_file="$home/profiles/headless/cordis.patch.yml"
  local attach_sock="/tmp/tmux-$(id -u)/default"

  if [[ -z "$result_file" ]]; then
    echo "warning: GENGIN_SESSION_RESULT_PATH is not set (not a supervised session)" >&2
    return 2
  fi
  if [[ ! -f "$patch_file" ]]; then
    echo "warning: session patch layer missing: $patch_file" >&2
    return 2
  fi
  # The TUI plugin owns its profile and dsh regenerates parts of it at boot;
  # give the session its own writable copy of the version-matched checkout
  # profile (a symlink would be read-only for the agent).
  mkdir -p "$home/profiles"
  rm -rf "$home/profiles/dsh-tui"
  if ! cp -a "$LLMOPT_DIR/.dsh/profiles/dsh-tui" "$home/profiles/dsh-tui"; then
    echo "warning: could not copy the dsh-tui profile into the session home" >&2
    return 2
  fi

  # One run at a time: take over the agent user's tmux server so the pane
  # inherits this session's environment (capped key, session variables).
  tmux kill-server 2>/dev/null || true
  if tmux has-session -t "$run_tmux" 2>/dev/null; then
    echo "warning: stale tmux session '$run_tmux' survived kill-server" >&2
    return 2
  fi
  if ! tmux new-session -d -s "$run_tmux" -x 220 -y 50 -c "$LLMOPT_DIR" \
    "DSH_HOME='$home' dsh-tui --patch '$patch_file' -- \"\$(cat '$QUERY_FILE_ARG')\""; then
    echo "warning: could not start the tmux session for the TUI" >&2
    tmux kill-server 2>/dev/null || true
    return 2
  fi
  # Ctrl-a is this TUI's prefix so detaching from the console pane (whose
  # prefix is Ctrl-b) is just Ctrl-a d.
  tmux set -g prefix C-a 2>/dev/null || true

  echo "tui:         live session UI: tmux session '$run_tmux'"
  echo "tui:         attach: gengin-tui  (or tmux -S $attach_sock attach -t $run_tmux)"
  echo "tui:         detach: Ctrl-a d  (the session keeps running)"

  trap 'tmux kill-server 2>/dev/null || true; exit 130' TERM INT HUP
  while :; do
    if [[ -f "$result_file" ]]; then
      echo "tui:         session reported: $(tr '\n' ' ' < "$result_file" | head -c 240)"
      break
    fi
    if ! tmux has-session -t "$run_tmux" 2>/dev/null; then
      echo "error: the TUI session exited before reporting a result" >&2
      return 1
    fi
    sleep 5
  done
  echo "tui:         session complete - the TUI stays open for reading"
  return 0
}

# A chain request changes how bare model words are interpreted, no matter
# where the flags appear relative to the models.
ROUTE_FLAG=0
for arg in "$@"; do
  case "$arg" in
    --max-context|--fallback-model) ROUTE_FLAG=1 ;;
  esac
done

while [[ $# -gt 0 ]]; do
  case "$1" in
    local)
      PRESET=local
      PROVIDER_ARGS=(--provider custom:local); shift ;;
    # `ml` only selects the ML prompt; the provider comes from the preset words
    # (in any order) and defaults to the local server when none was given.
    ml)
      ML_MODE=1; shift ;;
    openrouter|or)
      PRESET=openrouter
      PROVIDER_ARGS=(--provider openrouter); shift ;;
    deepseek|ds)
      if [[ "$ROUTE_FLAG" -eq 1 && ${#MODEL_ARGS[@]} -gt 0 && -z "$ROUTE_FALLBACK" ]]; then
        # Inside a chain the word names the fallback leg (direct DeepSeek API).
        ROUTE_FALLBACK="deepseek"; shift
      else
        PRESET=deepseek
        PROVIDER_ARGS=(--provider deepseek); shift
      fi ;;
    openai|oa)
      if [[ "$ROUTE_FLAG" -eq 1 && ${#MODEL_ARGS[@]} -gt 0 && -z "$ROUTE_FALLBACK" ]]; then
        # Inside a chain the word names the fallback leg (direct OpenAI API).
        ROUTE_FALLBACK="openai"; shift
      else
        PRESET=openai
        PROVIDER_ARGS=(--provider openai); shift
      fi ;;
    --model)
      [[ $# -ge 2 ]] || { echo "error: --model needs a value" >&2; exit 2; }
      MODEL="$2"; MODEL_ARGS=(-m "$2"); shift 2 ;;
    --goal)
      [[ $# -ge 2 ]] || { echo "error: --goal needs a value" >&2; exit 2; }
      GOAL="$2"; shift 2 ;;
    --headless)
      HEADLESS=1; shift ;;
    --web)
      WEB_REQ=1; shift ;;
    --tui)
      TUI_REQ=1; shift ;;
    --supervised)
      SUPERVISED=1; shift ;;
    --query-file)
      [[ $# -ge 2 ]] || { echo "error: --query-file needs a value" >&2; exit 2; }
      QUERY_FILE_ARG="$2"; shift 2 ;;
    --usage-file)
      [[ $# -ge 2 ]] || { echo "error: --usage-file needs a value" >&2; exit 2; }
      USAGE_FILE_ARG="$2"; shift 2 ;;
    --max-context)
      [[ $# -ge 2 ]] || { echo "error: --max-context needs a value" >&2; exit 2; }
      ROUTE_MAX_CONTEXT="$2"; shift 2 ;;
    --fallback-model)
      [[ $# -ge 2 ]] || { echo "error: --fallback-model needs a value" >&2; exit 2; }
      ROUTE_FALLBACK="$2"; shift 2 ;;
    --budget)
      [[ $# -ge 2 ]] || { echo "error: --budget needs a value" >&2; exit 2; }
      ROUTE_BUDGET="$2"; shift 2 ;;
    --wait-offpeak)
      WAIT_OFFPEAK=1; shift ;;
    --hermes)
      HARNESS=hermes; shift ;;
    --dsh)
      HARNESS=dsh; shift ;;
    --harness)
      [[ $# -ge 2 ]] || { echo "error: --harness needs a value (hermes|dsh)" >&2; exit 2; }
      HARNESS="$2"; shift 2 ;;
    --dry-run)
      DRY_RUN=1; shift ;;
    -h|--help)
      usage; exit 0 ;;
    -*)
      echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    *)
      if [[ "$ROUTE_FLAG" -eq 1 && ${#MODEL_ARGS[@]} -gt 0 && -z "$ROUTE_FALLBACK" ]]; then
        ROUTE_FALLBACK="$1"; shift  # second model in a chain = fallback leg
      else
        MODEL_ARGS=(-m "$1"); shift  # bare word = model id
      fi ;;
  esac
done

case "$HARNESS" in
  hermes|dsh) ;;
  *) echo "error: --harness expects hermes or dsh, got: $HARNESS" >&2; exit 2 ;;
esac

# Exported for child processes (the MCP server banner names its setup script).
export GENGIN_HARNESS="$HARNESS"

# dsh sandboxes shell commands (bwrap: no /dev, no X sockets) by default,
# which breaks the GPU benches; optimizer sessions need the full host access
# the Hermes harness always had.
if [[ "$HARNESS" == "dsh" ]]; then
  export DSH_PERMISSION_MODE="${DSH_PERMISSION_MODE:-danger-full-access}"
fi

# `--provider` is not accepted without `--model`; fall back to defaults
# per preset (override with GENGIN_<PRESET>_MODEL or a bare model argument).
if [[ "$ML_MODE" == "1" && ${#PROVIDER_ARGS[@]} -eq 0 && "$ROUTE_FLAG" -eq 0 ]]; then
  PRESET=local
  PROVIDER_ARGS=(--provider custom:local)
fi
if [[ -n "$PRESET" && ${#MODEL_ARGS[@]} -eq 0 ]]; then
  case "$PRESET" in
    local)      MODEL_ARGS=(-m "${GENGIN_LOCAL_MODEL:-Qwen3.8-27B}") ;;
    deepseek)   MODEL_ARGS=(-m "${GENGIN_DEEPSEEK_MODEL:-deepseek-v4-flash}") ;;
    openai)     MODEL_ARGS=(-m "${GENGIN_OPENAI_MODEL:-gpt-6.1-sol}") ;;
    openrouter) MODEL_ARGS=(-m "${GENGIN_OPENROUTER_MODEL:-deepseek/deepseek-v4-flash-0731}") ;;
  esac
fi

# UI selection: `local` (dsh) is interactive by default -> the dsh TUI. The
# Web UI is opt-in (--web), the one-shot via --headless; --hermes selects the
# Hermes TUI. Only one interactive surface per run.
if [[ "$TUI_REQ" -eq 1 ]]; then
  [[ "$HEADLESS" -eq 0 && "$WEB_REQ" -eq 0 && "$HARNESS" == "dsh" && "$PRESET" == "local" ]] || {
    echo "error: --tui needs the local preset on dsh and conflicts with --web/--headless/--hermes" >&2
    exit 2; }
fi
if [[ "$WEB_REQ" -eq 1 ]]; then
  [[ "$HEADLESS" -eq 0 ]] || { echo "error: --web cannot be combined with --headless" >&2; exit 2; }
  [[ "$HARNESS" == "dsh" ]] || { echo "error: --web is dsh-only (drop --hermes)" >&2; exit 2; }
  [[ "$PRESET" == "local" ]] || { echo "error: --web needs the local preset - run: $0 local --web" >&2; exit 2; }
fi

# --- best-effort harness self-update ---------------------------------------
# Keep the Hermes Agent checkout at origin/main: a cheap `update --check` on
# every launch, the full update only when commits are pending. Never fatal —
# a failed or slow update leaves the current version in place.
# GENGIN_SKIP_HARNESS_UPDATE=1 disables; GENGIN_HARNESS_UPDATE_TIMEOUT (seconds).
maybe_update_hermes() {
  [[ "${GENGIN_SKIP_HARNESS_UPDATE:-0}" == "1" ]] && return 0
  command -v hermes >/dev/null 2>&1 || return 0
  local log out budget
  log="${HERMES_HOME:-$HERMES_DIR}/harness-update.log"
  [[ -w "${log%/*}" ]] || log="/tmp/gengin-harness-update.log"
  out="$(timeout 120 hermes update --check 2>&1)" || true
  printf '%s\n' "$out" >>"$log" 2>/dev/null || true
  case "$out" in
    *"Update available"*|*"commits behind"*) ;;
    *) return 0 ;;
  esac
  echo "[harness] update available — installing the latest Hermes Agent (log: $log)"
  budget="${GENGIN_HARNESS_UPDATE_TIMEOUT:-420}"
  if timeout "$budget" hermes update --yes >>"$log" 2>&1; then
    echo "[harness] Hermes Agent updated"
  else
    echo "[harness] warning: harness update failed — continuing with the current version (log: $log)" >&2
  fi
  return 0
}

# --- supervised mode (launched by the supervisor, not a human) -------------
# Requires OPENROUTER_API_KEY in the inherited environment (the temporary
# capped key). Never reads KEY= from llmOpt/.env and never writes the key to
# .hermes/.env. Requires explicit model, query path, and usage path.
if [[ "$SUPERVISED" -eq 1 ]]; then
  if [[ -z "${OPENROUTER_API_KEY:-}" ]]; then
    echo "error: supervised mode requires OPENROUTER_API_KEY in the environment" >&2
    exit 2
  fi
  if [[ -z "$MODEL" || -z "$QUERY_FILE_ARG" || -z "$USAGE_FILE_ARG" ]]; then
    echo "error: supervised mode requires --model, --query-file, and --usage-file" >&2
    exit 2
  fi
  if [[ ! -f "$QUERY_FILE_ARG" ]]; then
    echo "error: query file not found: $QUERY_FILE_ARG" >&2
    exit 2
  fi
  # Use the supervisor-provided per-session home (isolated, credential-free).
  # $HERMES_DIR/$DSH_DIR are only a fallback for manual invocations.
  if [[ "$HARNESS" == "dsh" ]]; then
    DSH_HOME_RESOLVED="${DSH_HOME:-$DSH_DIR}"
    if [[ ! -d "$DSH_HOME_RESOLVED" ]]; then
      echo "error: DSH_HOME not found: $DSH_HOME_RESOLVED" >&2
      exit 2
    fi
    if ! command -v dsh >/dev/null 2>&1; then
      echo "error: dsh not installed (install: npm install -g @deepseek-ai/dsh)" >&2
      exit 127
    fi
  else
    HERMES_HOME_RESOLVED="${HERMES_HOME:-$HERMES_DIR}"
    if [[ ! -d "$HERMES_HOME_RESOLVED" ]]; then
      echo "error: HERMES_HOME not found: $HERMES_HOME_RESOLVED" >&2
      exit 2
    fi
  fi
  export PYTHONUNBUFFERED=1
  # Shared-group umask: files the agent creates must stay writable by the
  # supervisor (it snapshots/restores the sandbox and reads diffs).
  umask 002
  echo "=== gengin optimizer (supervised) ==="
  if [[ "$HARNESS" == "dsh" ]]; then
    echo "harness:     DeepSeek Harness (dsh)"
    echo "DSH_HOME:    $DSH_HOME_RESOLVED"
  else
    echo "harness:     Hermes Agent"
    echo "HERMES_HOME: $HERMES_HOME_RESOLVED"
  fi
  echo "model:       $MODEL"
  echo "query:       $QUERY_FILE_ARG"
  echo "usage:       $USAGE_FILE_ARG"
  echo
  cd "$LLMOPT_DIR"
  # No exec: the supervisor owns the process group and needs a stable wrapper
  # to reap the child and preserve its exit code.
  if [[ "$HARNESS" == "dsh" ]]; then
    # dsh has no --usage-file equivalent: the supervisor tracks spend through
    # the capped OpenRouter key (and the proxy cost log). Write a stub so any
    # reader of the path still sees valid JSON.
    printf '{"harness":"dsh","note":"usage is tracked via the capped OpenRouter key"}\n' \
      >"$USAGE_FILE_ARG"
    # Proprietary code: the OTel row is launcher-owned, so the opt-out is the
    # environment switch (the session-log row is disabled in the patch layer).
    export DSH_TELEMETRY_MODE=DISABLED DSH_TELEMETRY_DISABLED=1
    # Opt-in TUI-hosted sessions (GENGIN_SUPERVISED_TUI=1 in the environment
    # or llmOpt/.env): run inside a real dsh-tui in tmux so a human can watch
    # the session live; the supervisor contract (result.json, exit code,
    # budget, key cleanup) is unchanged.
    supervised_tui="${GENGIN_SUPERVISED_TUI:-}"
    if [[ -z "$supervised_tui" ]]; then
      supervised_tui="$(grep -E '^GENGIN_SUPERVISED_TUI=' "$LLMOPT_DIR/.env" 2>/dev/null | head -1 | cut -d= -f2- | tr -d '[:space:]' || true)"
    fi
    if [[ "$supervised_tui" == "1" ]] && command -v tmux >/dev/null 2>&1 \
        && command -v dsh-tui >/dev/null 2>&1; then
      launch_supervised_tui "$DSH_HOME_RESOLVED"
      tui_rc=$?
      if [[ "$tui_rc" -ne 2 ]]; then
        exit "$tui_rc"
      fi
      echo "warning: TUI hosting unavailable - running headless" >&2
    fi
    if [[ "$supervised_tui" == "1" ]]; then
      echo "warning: GENGIN_SUPERVISED_TUI=1 but tmux/dsh-tui is missing - running headless" >&2
    fi
    DSH_HOME="$DSH_HOME_RESOLVED" dsh --profile headless "$(cat "$QUERY_FILE_ARG")"
  else
    maybe_update_hermes
    HERMES_HOME="$HERMES_HOME_RESOLVED" hermes -z "$(cat "$QUERY_FILE_ARG")" --yolo \
      --usage-file "$USAGE_FILE_ARG" --provider openrouter -m "$MODEL"
  fi
  exit $?
fi

if [[ ! -f "$PROMPT_FILE" ]]; then
  echo "error: prompt file not found: $PROMPT_FILE" >&2
  exit 1
fi

if [[ "$ML_MODE" == "1" ]]; then
  PROMPT_FILE="$LLMOPT_DIR/prompts/optimize-ml.md"
  if ! nvidia-smi -L >/dev/null 2>&1; then
    echo "[ml] warning: no NVIDIA GPU detected - the layer bench needs a working OpenCL device" >&2
  else
    echo "[ml] GPU: benches auto-pin to the fastest free GPU (3090 first, 3060 while the 3090 is busy)"
  fi
  echo "[ml] objective: machineLearning/generateKernel.py layer kernels (suites smoke/core/edges/upscale/srnet)"
  echo "[ml] provider: ${PRESET:-default} model: ${MODEL_ARGS[1]:-default}"
fi

# Manual runs use the rendered harness home; supervised runs get their own
# per-session home from the supervisor.
if [[ "$HARNESS" == "dsh" ]]; then
  HARNESS_DIR="$DSH_DIR"
else
  HARNESS_DIR="$HERMES_DIR"
fi
if [[ ! -d "$HARNESS_DIR" ]]; then
  if [[ "$HARNESS" == "dsh" ]]; then
    echo "error: $HARNESS_DIR not found - run $SCRIPT_DIR/setup-dsh.sh first" >&2
    exit 1
  fi
  echo "error: $HERMES_DIR not found — run $SCRIPT_DIR/setup-hermes.sh first" >&2
  exit 1
fi

# Expose cloud keys to Hermes from the single secret source (if not already set).
if [[ -z "${OPENROUTER_API_KEY:-}" ]]; then
  KEY_VALUE="$(grep -E '^KEY=' "$LLMOPT_DIR/.env" 2>/dev/null | head -1 | cut -d= -f2- || true)"
  if [[ -n "$KEY_VALUE" ]]; then
    export OPENROUTER_API_KEY="$KEY_VALUE"
  fi
fi

# --- cost-aware route: switch models when the prompt crosses a threshold ---
# CLI flags win over llmOpt/.env; both halves are required (primary = the
# session model, fallback = the model the proxy switches to). The proxy
# counts with OpenRouter's own response usage, so no extra key is needed.
route_max_context="${ROUTE_MAX_CONTEXT:-}"
route_fallback="${ROUTE_FALLBACK:-}"
if [[ -z "$route_max_context" ]]; then
  route_max_context="$(grep -E '^GENGIN_ROUTE_MAX_CONTEXT=' "$LLMOPT_DIR/.env" 2>/dev/null | head -1 | cut -d= -f2- || true)"
fi
if [[ -z "$route_fallback" ]]; then
  route_fallback="$(grep -E '^GENGIN_ROUTE_FALLBACK_MODEL=' "$LLMOPT_DIR/.env" 2>/dev/null | head -1 | cut -d= -f2- || true)"
fi
route_ttl="$(grep -E '^GENGIN_ROUTE_TTL_SECONDS=' "$LLMOPT_DIR/.env" 2>/dev/null | head -1 | cut -d= -f2- || true)"
route_ttl="${route_ttl:-86400}"
route_budget="${ROUTE_BUDGET:-}"
if [[ -z "$route_budget" ]]; then
  route_budget="$(grep -E '^GENGIN_ROUTE_BUDGET_USD=' "$LLMOPT_DIR/.env" 2>/dev/null | head -1 | cut -d= -f2- || true)"
fi
route_enabled=0
if [[ -n "$route_max_context" || -n "$route_fallback" ]]; then
  if [[ "$PRESET" == "local" || "$PRESET" == "deepseek" || "$PRESET" == "openai" ]]; then
    if [[ "$ROUTE_FLAG" -eq 1 ]]; then
      echo "error: a chain needs OpenRouter models — the '${PRESET}' preset bypasses the proxy (chain syntax: PRIMARY --max-context 99K FALLBACK)" >&2
      exit 2
    fi
    echo "warning: GENGIN_ROUTE_* is set but the '${PRESET}' preset bypasses the proxy — route disabled" >&2
  elif [[ -z "$route_max_context" || -z "$route_fallback" ]]; then
    echo "error: a route needs both a threshold and a fallback model (use --max-context + --fallback-model, or GENGIN_ROUTE_MAX_CONTEXT + GENGIN_ROUTE_FALLBACK_MODEL in .env)" >&2
    exit 2
  else
    route_enabled=1
  fi
fi

if [[ "$route_enabled" -eq 1 ]]; then
  # `deepseek`/`openai` as the fallback word mean the DIRECT vendor API: the
  # proxy holds a second upstream for that leg, so the key rides along with
  # the loopback registration.  Any other word is an OpenRouter model id.
  route_fallback_upstream=""
  route_direct_key=""
  route_key_name=""
  case "$route_fallback" in
    deepseek|ds)
      route_fallback_upstream="deepseek"
      route_fallback="${GENGIN_DEEPSEEK_MODEL:-deepseek-v4-flash}"
      route_key_name="DEEPSEEK_API_KEY"
      ;;
    openai|oa)
      route_fallback_upstream="openai"
      route_fallback="${GENGIN_OPENAI_MODEL:-gpt-6.1-sol}"
      route_key_name="OPENAI_API_KEY"
      ;;
  esac
  if [[ -n "$route_fallback_upstream" ]]; then
    route_direct_key="${!route_key_name:-}"
    if [[ -z "$route_direct_key" ]]; then
      route_direct_key="$(grep -E "^${route_key_name}=" "$LLMOPT_DIR/.env" 2>/dev/null | head -1 | cut -d= -f2- || true)"
    fi
    if [[ -z "$route_direct_key" ]]; then
      echo "error: the $route_fallback_upstream fallback needs $route_key_name (environment or llmOpt/.env)" >&2
      exit 2
    fi
    [[ "$route_direct_key" =~ ^[A-Za-z0-9._-]+$ ]] || {
      echo "error: $route_key_name contains unexpected characters" >&2; exit 2; }
  else
    [[ "$route_fallback" =~ ^[A-Za-z0-9._:/-]+$ ]] || {
      echo "error: invalid fallback model: $route_fallback" >&2; exit 2; }
  fi
  route_primary="${MODEL:-${MODEL_ARGS[1]:-}}"
  if [[ -z "$route_primary" ]]; then
    route_primary="${GENGIN_OPENROUTER_MODEL:-deepseek/deepseek-v4-flash-0731}"
    MODEL_ARGS=(-m "$route_primary")
  fi
  [[ "$route_primary" =~ ^[A-Za-z0-9._:/-]+$ ]] || {
    echo "error: invalid primary model: $route_primary" >&2; exit 2; }
  [[ "$route_ttl" =~ ^[0-9]+$ ]] && [ "$route_ttl" -ge 60 ] && [ "$route_ttl" -le 604800 ] || {
    echo "error: GENGIN_ROUTE_TTL_SECONDS must be 60..604800 seconds: $route_ttl" >&2; exit 2; }
  if [[ -n "$route_budget" ]]; then
    [[ "$route_budget" =~ ^[0-9]+([.][0-9]+)?$ ]] || {
      echo "error: --budget expects a positive USD amount, got: $route_budget" >&2; exit 2; }
    awk "BEGIN{exit !($route_budget > 0)}" || {
      echo "error: --budget must be greater than 0, got: $route_budget" >&2; exit 2; }
  fi
  if [[ "$route_fallback_upstream" == "deepseek" ]]; then
    # DeepSeek halves its prices outside the peak windows: warn, and optionally
    # wait, so a long session does not run at full price.
    offpeak_rc=0
    offpeak_out="$(python3 "$LLMOPT_DIR/deepseek_pricing.py" status 2>&1)" || offpeak_rc=$?
    if [[ "$offpeak_rc" -ne 0 ]]; then
      echo "[off-peak] $offpeak_out" >&2
      offpeak_env_file="$(grep -E '^GENGIN_OFFPEAK_WAIT=' "$LLMOPT_DIR/.env" 2>/dev/null | head -1 | cut -d= -f2- || true)"
      if [[ "$WAIT_OFFPEAK" -eq 1 || "${GENGIN_OFFPEAK_WAIT:-$offpeak_env_file}" == "1" ]]; then
        if [[ "$DRY_RUN" -eq 1 ]]; then
          echo "[off-peak] dry-run: would wait for the half-price window"
        else
          echo "[off-peak] waiting for the half-price window..." >&2
          python3 "$LLMOPT_DIR/deepseek_pricing.py" wait || echo "[off-peak] warning: wait failed - continuing" >&2
        fi
      else
        echo "[off-peak] tip: --wait-offpeak starts the session in the half-price window" >&2
      fi
    elif [[ -n "$offpeak_out" ]]; then
      echo "[off-peak] $offpeak_out"
    fi
  fi
  route_tokens="$(parse_tokens "$route_max_context")" || {
    echo "error: --max-context expects a token count like 99000 or 99K, got: $route_max_context" >&2
    exit 2; }
  if [[ ${#PROVIDER_ARGS[@]} -eq 0 ]]; then
    PRESET=openrouter
    PROVIDER_ARGS=(--provider openrouter)
  fi
  if [[ -n "$route_fallback_upstream" ]]; then
    route_fallback_member="$(printf '{"model":"%s","upstream":"%s"}' "$route_fallback" "$route_fallback_upstream")"
    route_fallback_label="$route_fallback (direct $route_fallback_upstream API)"
  else
    route_fallback_member="\"$route_fallback\""
    route_fallback_label="$route_fallback"
  fi
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[route] chain: $route_primary -> $route_fallback_label once the prompt reaches $route_tokens tokens"
  else
    PROXY_PORT="${GENGIN_PROXY_PORT:-8787}"
    if ! ensure_route_proxy "$PROXY_PORT"; then
      echo "error: OpenRouter proxy not reachable on http://127.0.0.1:$PROXY_PORT — start it with $SCRIPT_DIR/setup-openrouter-proxy.sh" >&2
      exit 2
    fi
    route_json="$(printf '{"members":["%s",%s],"maxContext":%s,"ttlSeconds":%s,"label":"gengin-opt"%s%s}' \
      "$route_primary" "$route_fallback_member" "$route_tokens" "$route_ttl" \
      "${route_direct_key:+,\"${route_fallback_upstream}Key\":\"$route_direct_key\"}" \
      "${route_budget:+,\"budgetUsd\":$route_budget}")"
    if ! curl -fsS --max-time 5 -X POST -H 'Content-Type: application/json' \
        -d "$route_json" "http://127.0.0.1:$PROXY_PORT/route/register" >/dev/null 2>&1; then
      echo "error: route registration failed on http://127.0.0.1:$PROXY_PORT/route/register (a proxy running before the route feature must be restarted: systemctl --user restart gengin-openrouter-proxy)" >&2
      exit 2
    fi
    echo "[route] $route_primary -> $route_fallback_label at $route_tokens prompt tokens (ttl ${route_ttl}s${route_budget:+, budget \$$route_budget})"
  fi
fi

# OpenRouter traffic goes through the local filtering proxy (model.base_url in
# the rendered Hermes config).  Warn early when it is not running.
if [[ "$PRESET" != "local" && "$PRESET" != "deepseek" && "$PRESET" != "openai" ]]; then
  PROXY_PORT="${GENGIN_PROXY_PORT:-8787}"
  if ! curl -fsS --max-time 2 "http://127.0.0.1:$PROXY_PORT/health" >/dev/null 2>&1; then
    echo "warning: OpenRouter proxy not reachable on http://127.0.0.1:$PROXY_PORT — start it with llmOpt/scripts/setup-openrouter-proxy.sh" >&2
  fi
fi
if [[ -z "${DEEPSEEK_API_KEY:-}" ]]; then
  DEEPSEEK_VALUE="$(grep -E '^DEEPSEEK_API_KEY=' "$LLMOPT_DIR/.env" 2>/dev/null | head -1 | cut -d= -f2- || true)"
  if [[ -n "$DEEPSEEK_VALUE" ]]; then
    export DEEPSEEK_API_KEY="$DEEPSEEK_VALUE"
  fi
fi
if [[ -z "${OPENAI_API_KEY:-}" ]]; then
  OPENAI_VALUE="$(grep -E '^OPENAI_API_KEY=' "$LLMOPT_DIR/.env" 2>/dev/null | head -1 | cut -d= -f2- || true)"
  if [[ -n "$OPENAI_VALUE" ]]; then
    export OPENAI_API_KEY="$OPENAI_VALUE"
  fi
fi

# create_pr runs inside the MCP server, which inherits this environment.
if [[ -z "${GITHUB_TOKEN:-}" ]]; then
  GITHUB_VALUE="$(grep -E '^GITHUB_TOKEN=' "$LLMOPT_DIR/.env" 2>/dev/null | head -1 | cut -d= -f2- || true)"
  if [[ -n "$GITHUB_VALUE" ]]; then
    export GITHUB_TOKEN="$GITHUB_VALUE"
  fi
fi

# Open PRs are already-proposed changes; manual sessions get the same context the
# supervisor injects.  Best effort: no token or no network leaves it empty.
PR_BLOCK="$(LLMOPT_DIR="$LLMOPT_DIR" python3 - <<'PY' 2>/dev/null
import os, sys
sys.path.insert(0, os.environ["LLMOPT_DIR"])
try:
    import main
    # main.PROJECT_DIR is relative at import time; the GitHub remote lives in the
    # repository checkout, not in a subdirectory of the launcher.
    main.PROJECT_DIR = os.path.abspath(os.path.join(os.environ["LLMOPT_DIR"], ".."))
    prs = main.openPullRequests(limit=10)
except Exception:
    prs = []
if prs:
    print("Open pull requests - these changes are already proposed; do not")
    print("re-implement one of them (see the prompt rules):")
    for pr in prs:
        files = ", ".join(pr.get("files") or []) or "(files unavailable)"
        print("  #%s %s [%s] files: %s" % (pr.get("number"), (pr.get("title") or "")[:90],
                                           pr.get("branch") or "?", files[:220]))
    print()
PY
)" || PR_BLOCK=""

mkdir -p "$HARNESS_DIR/cache"
QUERY_FILE="$HARNESS_DIR/cache/query-$(date +%s).md"
{
  cat "$PROMPT_FILE"
  echo
  echo "---"
  if [[ -n "$PR_BLOCK" ]]; then
    printf '%s\n' "$PR_BLOCK"
  fi
  if [[ -n "$GOAL" ]]; then
    echo "Session goal: $GOAL"
    echo
  fi
  if [[ "$ML_MODE" == "1" ]]; then
    echo "Start now. Work the measure -> change the generator -> re-measure -> validate -> PR loop."
  else
    echo "Start now. Work the profile -> micro-benchmark -> pre-mortem -> apply -> validate -> PR loop."
  fi
} > "$QUERY_FILE"
if [[ "$HARNESS" == "dsh" ]]; then
  # dsh reads the model and endpoint from a rendered profile patch layer under
  # DSH_HOME; provider/model CLI flags are Hermes-only. The local preset is
  # honored: it renders profiles/local (headless one-shot) and
  # profiles/local-web (the interactive Web UI) against the local router
  # instead of the headless profile (OpenRouter proxy route). Interactive
  # local runs boot the Web UI so the session can be steered; --headless
  # keeps the auto-run one-shot.
  DSH_PROFILE="headless"
  TUI_MODE=0
  if [[ "$PRESET" == "local" ]]; then
    export LOCAL_LLM_KEY="${LOCAL_LLM_KEY:-none}"
    if [[ "$HEADLESS" -eq 1 ]]; then
      DSH_PROFILE="local"
      render_local_dsh_profile "$HARNESS_DIR" "$DSH_PROFILE"
    elif [[ "$WEB_REQ" -eq 1 ]]; then
      DSH_PROFILE="local-web"
      WEB_MODE=1
      WEB_PORT="${GENGIN_WEB_PORT:-3081}"
      if [[ ! "$WEB_PORT" =~ ^[0-9]+$ ]] || (( 10#$WEB_PORT < 1024 || 10#$WEB_PORT > 65535 )); then
        echo "error: GENGIN_WEB_PORT must be a port number 1024..65535, got: $WEB_PORT" >&2
        exit 2
      fi
      render_local_dsh_profile "$HARNESS_DIR" "$DSH_PROFILE"
    else
      # Interactive default: the dsh TUI (a terminal UI plugin for dsh). The
      # plugin profile is initialized once into DSH_HOME; our wiring rides as
      # a regenerated --patch overlay so the TUI keeps owning its own files.
      # Boot through the dsh-tui LAUNCHER (it tags the launch for the plugin);
      # the plain `dsh --profile dsh-tui` route silently skips the frontend
      # whenever stdout is not a TTY (capture hooks, pipes) - no error, no UI.
      DSH_PROFILE="dsh-tui"
      TUI_MODE=1
      if command -v dsh-tui >/dev/null 2>&1; then
        TUI_LAUNCHER=(dsh-tui)
      elif [[ -f "$HARNESS_DIR/profiles/dsh-tui/node_modules/@deepseek-harness-tui/dsh-tui/bin/dsh-tui.js" ]]; then
        # The global command is a shim over exactly this profile copy.
        TUI_LAUNCHER=(node "$HARNESS_DIR/profiles/dsh-tui/node_modules/@deepseek-harness-tui/dsh-tui/bin/dsh-tui.js")
      else
        echo "error: the dsh TUI is not installed (no dsh-tui command, no profile copy)" >&2
        echo "  install: npm install -g @deepseek-harness-tui/dsh-tui" >&2
        echo "           DSH_HOME=\"$HARNESS_DIR\" dsh plugin --profile dsh-tui add @deepseek-harness-tui/dsh-tui" >&2
        echo "  or use:  $0 local --web | $0 local --hermes | $0 local --headless" >&2
        exit 1
      fi
      render_local_tui_overlay "$HARNESS_DIR"
    fi
  elif [[ ${#PROVIDER_ARGS[@]} -gt 0 ]]; then
    echo "note: --provider/-m are ignored on dsh — set GENGIN_OPENROUTER_MODEL and re-run setup-dsh.sh" >&2
  fi
  if [[ "$WEB_MODE" -eq 1 ]]; then
    CMD=(dsh --profile "$DSH_PROFILE" --no-open --port "$WEB_PORT")
    DISPLAY_CMD=(dsh --profile "$DSH_PROFILE" --no-open --port "$WEB_PORT")
  elif [[ "$TUI_MODE" -eq 1 ]]; then
    # dsh-tui takes host options before `--` and the literal prompt after it;
    # the plugin auto-submits that prompt once the UI is up.
    CMD=("${TUI_LAUNCHER[@]}" --patch "$TUI_OVERLAY" -- "$(cat "$QUERY_FILE")")
    DISPLAY_CMD=("${TUI_LAUNCHER[@]}" --patch "$TUI_OVERLAY" -- "<query: $(wc -c <"$QUERY_FILE") bytes>")
  else
    CMD=(dsh --profile "$DSH_PROFILE" "$(cat "$QUERY_FILE")")
    DISPLAY_CMD=(dsh --profile "$DSH_PROFILE" "<query: $(wc -c <"$QUERY_FILE") bytes>")
  fi
elif [[ "$HEADLESS" -eq 1 ]]; then
  # `-z` is the scripted one-shot entry point (final answer only) and the only
  # form that supports --usage-file.
  USAGE_FILE="$HARNESS_DIR/cache/usage-$(date +%s).json"
  CMD=(hermes -z "$(cat "$QUERY_FILE")" --yolo --usage-file "$USAGE_FILE")
  DISPLAY_CMD=(hermes -z "<query: $(wc -c <"$QUERY_FILE") bytes>" --yolo --usage-file "$USAGE_FILE")
else
  CMD=(hermes chat --source tool --query-file "$QUERY_FILE")
  DISPLAY_CMD=("${CMD[@]}")
fi

if [[ "$DRY_RUN" -eq 1 ]]; then
  if [[ "$HARNESS" == "dsh" ]]; then
    if [[ "$WEB_MODE" -eq 1 ]]; then
      echo "# local Web UI mode: no browser popup; start the session in the UI with"
      echo "# the brief at $HARNESS_DIR/cache/query-latest.md (written at launch)"
    elif [[ "$TUI_MODE" -eq 1 ]]; then
      echo "# local TUI mode: the session brief is the first prompt; overlay at"
      echo "# $TUI_OVERLAY"
    fi
    printf 'DSH_HOME=%q ' "$HARNESS_DIR"
    printf '%q ' "${DISPLAY_CMD[@]}"
  else
    printf 'HERMES_HOME=%q ' "$HARNESS_DIR"
    printf '%q ' "${DISPLAY_CMD[@]}" "${PROVIDER_ARGS[@]}" "${MODEL_ARGS[@]}"
  fi
  echo
  exit 0
fi

if [[ "$HARNESS" == "dsh" ]]; then
  if ! command -v dsh >/dev/null 2>&1; then
    echo "error: dsh not installed" >&2
    echo "  install: npm install -g @deepseek-ai/dsh  (Node.js 18+; developer preview)" >&2
    exit 127
  fi
else
  if ! command -v hermes >/dev/null 2>&1; then
    echo "error: hermes not installed" >&2
    echo "  install: curl -fsSL https://raw.githubusercontent.com/NousResearch/hermes-agent/main/scripts/install.sh | bash" >&2
    exit 127
  fi
fi

# run_perf_stat / make_flame need unprivileged perf counters; offer the
# one-time sudo fix before starting a session.
if ! perf stat -e cycles true >/dev/null 2>&1; then
  if [[ "$HEADLESS" -eq 0 && -t 0 && -t 1 ]]; then
    echo "perf counters are blocked (perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid))."
    read -r -p "Enable them now via sudo? [y/N] " answer
    if [[ "$answer" == [yY] ]]; then
      "$SCRIPT_DIR/enable-perf.sh" || echo "warning: perf could not be enabled — continuing without counters" >&2
    fi
  else
    echo "note: perf counters blocked — run $SCRIPT_DIR/enable-perf.sh once (needs sudo)" >&2
  fi
fi

# A session must start from the newest commit: advance the sandbox to the remote
# tip of the target branch before the harness boots.  Supervised sessions skip
# this — the supervisor pinned GENGIN_TARGET_SHA and baselined that commit.
if [[ -z "${GENGIN_TARGET_SHA:-}" && "${GENGIN_SKIP_SANDBOX_SYNC:-0}" != "1" ]]; then
  LLMOPT_DIR="$LLMOPT_DIR" python3 - <<'PY'
import os, sys
sys.path.insert(0, os.environ["LLMOPT_DIR"])
try:
    import main
    main.PROJECT_DIR = os.path.join(os.environ["LLMOPT_DIR"], "gengin")
    result = main.syncSandboxToLatest(
        os.environ.get("GENGIN_REPO_URL", "git@github.com:DarkBenky/gengin.git"),
        os.environ.get("GENGIN_TARGET_BRANCH", "main"),
        session_id=os.environ.get("GENGIN_SESSION_ID", "manual"),
        inputs_dir=os.environ.get("GENGIN_INPUTS_DIR") or None,
    )
    if result["action"] == "dirty":
        print("warning: sandbox has local changes - keeping its current base", file=sys.stderr)
    else:
        print(f"sandbox base: {result['sha'][:12]} ({result['action']})")
except Exception as exc:
    print(f"warning: sandbox sync to latest failed: {exc}", file=sys.stderr)
PY
fi

if [[ "$HARNESS" != "dsh" ]]; then
  maybe_update_hermes
fi

if [[ "$HARNESS" == "dsh" ]]; then
  # Proprietary code: opt out of the launcher-owned OTel row (see above).
  export DSH_TELEMETRY_MODE=DISABLED DSH_TELEMETRY_DISABLED=1
  if [[ "$WEB_MODE" -eq 1 ]]; then
    if (exec 3<>"/dev/tcp/127.0.0.1/$WEB_PORT") 2>/dev/null; then
      if [[ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:$WEB_PORT/" || true)" == "401" ]]; then
        echo "note: a local web UI is already running on :$WEB_PORT - nothing to start."
        echo "Open it:  http://127.0.0.1:$WEB_PORT/?token=$(cat "$HARNESS_DIR/web-launch-token" 2>/dev/null || true)"
        echo "Stop it (Ctrl+C in its terminal), or use another port:"
        echo "  GENGIN_WEB_PORT=3082 $0 local --web"
        exit 0
      fi
      echo "error: port $WEB_PORT is already in use by something else - use GENGIN_WEB_PORT" >&2
      exit 2
    fi
    source "$SCRIPT_DIR/dsh-web-common.sh"
    dshWebPrepareLaunch "$HARNESS_DIR"
    BRIEF_FILE="$HARNESS_DIR/cache/query-latest.md"
    cp "$QUERY_FILE" "$BRIEF_FILE"
    echo "=== gengin optimizer (DeepSeek Harness, Web UI) ==="
    echo "DSH_HOME:  $HARNESS_DIR"
    echo "Profile:   $DSH_PROFILE"
    echo "Web UI:    http://127.0.0.1:$WEB_PORT"
    echo "Bookmark:  http://127.0.0.1:$WEB_PORT/?token=$DSH_LAUNCH_TOKEN"
    echo "Brief:     $BRIEF_FILE"
    echo
    echo "The browser is not opened for you. Open the bookmark, start a session,"
    echo "and send this first message:"
    echo
    echo "  Read $BRIEF_FILE and execute the work it describes."
    echo
    echo "Note: the UI starts in its own default workspace - use \"Choose workspace\""
    echo "to add $(dirname "$LLMOPT_DIR") so sessions run from the checkout."
    echo
    echo "Ctrl+C stops the host (a running turn is aborted); sessions persist and"
    echo "are resumable from the next local run."
    echo
    cd "$LLMOPT_DIR"
    DSH_HOME="$HARNESS_DIR" exec "${CMD[@]}"
  fi
  echo "=== gengin optimizer (DeepSeek Harness) ==="
  echo "DSH_HOME:  $HARNESS_DIR"
  echo "Profile:   ${DSH_PROFILE:-headless}"
  echo "Query:     $QUERY_FILE"
  if [[ "$TUI_MODE" -eq 1 ]]; then
    echo "Mode:      dsh TUI (first launch shows its own setup wizard)"
    echo "Overlay:   $TUI_OVERLAY"
    echo "If it starts without the brief, paste: Read $QUERY_FILE and execute the work it describes."
    ensure_tty_stdout
  fi
  echo
  cd "$LLMOPT_DIR"
  DSH_HOME="$HARNESS_DIR" exec "${CMD[@]}"
else
  echo "=== gengin optimizer (Hermes) ==="
  echo "HERMES_HOME: $HARNESS_DIR"
  echo "Query:       $QUERY_FILE"
  echo
  if [[ "$HEADLESS" -eq 0 ]]; then
    ensure_tty_stdout
  fi
  cd "$LLMOPT_DIR"
  HERMES_HOME="$HARNESS_DIR" exec "${CMD[@]}" "${PROVIDER_ARGS[@]}" "${MODEL_ARGS[@]}"
fi

