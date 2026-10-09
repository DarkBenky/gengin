#!/usr/bin/env bash
# Launch a gengin optimization session under the Hermes Agent harness.
#
#   gengin-opt.sh                       default model from .hermes/config.yaml
#   gengin-opt.sh openrouter [model]    force OpenRouter (optionally override model)
#   gengin-opt.sh local [model]         local llama.cpp server on :8013
#   gengin-opt.sh deepseek [model]      direct DeepSeek API (needs DEEPSEEK_API_KEY)
#   gengin-opt.sh openrouter PRIMARY --max-context 99K FALLBACK
#                                       switch to FALLBACK at 99K prompt tokens
#   gengin-opt.sh ml [local|openrouter|deepseek] [model]  ML layer objective
#   gengin-opt.sh --goal "speed up X"   append a session goal to the prompt
#   gengin-opt.sh --headless            scripted oneshot (-z), no approvals, usage report
#   gengin-opt.sh --dry-run             print the resolved command and exit
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLMOPT_DIR="$(dirname "$SCRIPT_DIR")"
HERMES_DIR="$LLMOPT_DIR/.hermes"
PROMPT_FILE="$LLMOPT_DIR/prompts/optimize.md"

PROVIDER_ARGS=()
MODEL_ARGS=()
PRESET=""
HEADLESS=0
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

usage() {
  echo "usage: $0 [local|openrouter|deepseek] [model] [--goal TEXT] [--headless] [--dry-run]"
  echo "       $0 ml [local|openrouter|deepseek] [model] [--goal TEXT] [--headless]"
  echo "       $0 openrouter PRIMARY --max-context 99K FALLBACK [--goal TEXT]"
  echo "       $0 openrouter PRIMARY --max-context 99K --budget 15 deepseek"
  echo "       $0 openrouter --supervised --model M --query-file F --usage-file U"
  echo "       --budget USD session spend cap; --wait-offpeak waits for half-price hours"
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
    --model)
      [[ $# -ge 2 ]] || { echo "error: --model needs a value" >&2; exit 2; }
      MODEL="$2"; MODEL_ARGS=(-m "$2"); shift 2 ;;
    --goal)
      [[ $# -ge 2 ]] || { echo "error: --goal needs a value" >&2; exit 2; }
      GOAL="$2"; shift 2 ;;
    --headless)
      HEADLESS=1; shift ;;
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
    openrouter) MODEL_ARGS=(-m "${GENGIN_OPENROUTER_MODEL:-deepseek/deepseek-v4-flash-0731}") ;;
  esac
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
  # $HERMES_DIR is only a fallback for manual invocations.
  HERMES_HOME_RESOLVED="${HERMES_HOME:-$HERMES_DIR}"
  if [[ ! -d "$HERMES_HOME_RESOLVED" ]]; then
    echo "error: HERMES_HOME not found: $HERMES_HOME_RESOLVED" >&2
    exit 2
  fi
  export PYTHONUNBUFFERED=1
  # Shared-group umask: files the agent creates must stay writable by the
  # supervisor (it snapshots/restores the sandbox and reads diffs).
  umask 002
  echo "=== gengin optimizer (supervised) ==="
  echo "HERMES_HOME: $HERMES_HOME_RESOLVED"
  echo "model:       $MODEL"
  echo "query:       $QUERY_FILE_ARG"
  echo "usage:       $USAGE_FILE_ARG"
  echo
  cd "$LLMOPT_DIR"
  maybe_update_hermes
  # No exec: the supervisor owns the process group and needs a stable wrapper
  # to reap the child and preserve its exit code.
  HERMES_HOME="$HERMES_HOME_RESOLVED" hermes -z "$(cat "$QUERY_FILE_ARG")" --yolo \
    --usage-file "$USAGE_FILE_ARG" --provider openrouter -m "$MODEL"
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

if [[ ! -d "$HERMES_DIR" ]]; then
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
  if [[ "$PRESET" == "local" || "$PRESET" == "deepseek" ]]; then
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
  # `deepseek` as the fallback word means the DIRECT DeepSeek API: the proxy
  # holds a second upstream for that leg, so the key rides along with the
  # loopback registration.  Any other word is an OpenRouter model id.
  route_fallback_direct=0
  route_ds_key=""
  case "$route_fallback" in
    deepseek|ds)
      route_fallback_direct=1
      route_fallback="${GENGIN_DEEPSEEK_MODEL:-deepseek-v4-flash}"
      route_ds_key="${DEEPSEEK_API_KEY:-}"
      if [[ -z "$route_ds_key" ]]; then
        route_ds_key="$(grep -E '^DEEPSEEK_API_KEY=' "$LLMOPT_DIR/.env" 2>/dev/null | head -1 | cut -d= -f2- || true)"
      fi
      if [[ -z "$route_ds_key" ]]; then
        echo "error: the deepseek fallback needs DEEPSEEK_API_KEY (environment or llmOpt/.env)" >&2
        exit 2
      fi
      [[ "$route_ds_key" =~ ^[A-Za-z0-9._-]+$ ]] || {
        echo "error: DEEPSEEK_API_KEY contains unexpected characters" >&2; exit 2; }
      ;;
  esac
  if [[ "$route_fallback_direct" -eq 0 ]]; then
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
  if [[ "$route_fallback_direct" -eq 1 ]]; then
    # DeepSeek halves its prices outside the peak windows: warn, and optionally
    # wait, so a long session does not run at full price.
    offpeak_rc=0
    offpeak_out="$(python3 "$LLMOPT_DIR/deepseek_pricing.py" status 2>&1)" || offpeak_rc=$?
    if [[ "$offpeak_rc" -ne 0 ]]; then
      echo "[off-peak] $offpeak_out" >&2
      if [[ "$WAIT_OFFPEAK" -eq 1 || "${GENGIN_OFFPEAK_WAIT:-0}" == "1" ]]; then
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
  if [[ "$route_fallback_direct" -eq 1 ]]; then
    route_fallback_member="$(printf '{"model":"%s","upstream":"deepseek"}' "$route_fallback")"
    route_fallback_label="$route_fallback (direct DeepSeek API)"
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
      "${route_ds_key:+,\"deepseekKey\":\"$route_ds_key\"}" \
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
if [[ "$PRESET" != "local" && "$PRESET" != "deepseek" ]]; then
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

mkdir -p "$HERMES_DIR/cache"
QUERY_FILE="$HERMES_DIR/cache/query-$(date +%s).md"
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
if [[ "$HEADLESS" -eq 1 ]]; then
  # `-z` is the scripted one-shot entry point (final answer only) and the only
  # form that supports --usage-file.
  USAGE_FILE="$HERMES_DIR/cache/usage-$(date +%s).json"
  CMD=(hermes -z "$(cat "$QUERY_FILE")" --yolo --usage-file "$USAGE_FILE")
  DISPLAY_CMD=(hermes -z "<query: $(wc -c <"$QUERY_FILE") bytes>" --yolo --usage-file "$USAGE_FILE")
else
  CMD=(hermes chat --source tool --query-file "$QUERY_FILE")
  DISPLAY_CMD=("${CMD[@]}")
fi

if [[ "$DRY_RUN" -eq 1 ]]; then
  printf 'HERMES_HOME=%q ' "$HERMES_DIR"
  printf '%q ' "${DISPLAY_CMD[@]}" "${PROVIDER_ARGS[@]}" "${MODEL_ARGS[@]}"
  echo
  exit 0
fi

if ! command -v hermes >/dev/null 2>&1; then
  echo "error: hermes not installed" >&2
  echo "  install: curl -fsSL https://raw.githubusercontent.com/NousResearch/hermes-agent/main/scripts/install.sh | bash" >&2
  exit 127
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

maybe_update_hermes

echo "=== gengin optimizer (Hermes) ==="
echo "HERMES_HOME: $HERMES_DIR"
echo "Query:       $QUERY_FILE"
echo
cd "$LLMOPT_DIR"
HERMES_HOME="$HERMES_DIR" exec "${CMD[@]}" "${PROVIDER_ARGS[@]}" "${MODEL_ARGS[@]}"
