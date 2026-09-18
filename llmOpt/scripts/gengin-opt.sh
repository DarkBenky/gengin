#!/usr/bin/env bash
# Launch a gengin optimization session under the Hermes Agent harness.
#
#   gengin-opt.sh                       default model from .hermes/config.yaml
#   gengin-opt.sh openrouter [model]    force OpenRouter (optionally override model)
#   gengin-opt.sh local [model]         local llama.cpp server on :8012
#   gengin-opt.sh deepseek [model]      direct DeepSeek API (needs DEEPSEEK_API_KEY)
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
SUPERVISED=0
MODEL=""
QUERY_FILE_ARG=""
USAGE_FILE_ARG=""

usage() {
  echo "usage: $0 [local|openrouter|deepseek] [model] [--goal TEXT] [--headless] [--dry-run]"
  echo "       $0 openrouter --supervised --model M --query-file F --usage-file U"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    local)
      PRESET=local
      PROVIDER_ARGS=(--provider custom:local); shift ;;
    openrouter|or)
      PRESET=openrouter
      PROVIDER_ARGS=(--provider openrouter); shift ;;
    deepseek|ds)
      PRESET=deepseek
      PROVIDER_ARGS=(--provider deepseek); shift ;;
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
    --dry-run)
      DRY_RUN=1; shift ;;
    -h|--help)
      usage; exit 0 ;;
    -*)
      echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    *)
      MODEL_ARGS=(-m "$1"); shift ;;  # bare word = model id
  esac
done

# `--provider` is not accepted without `--model`; fall back to defaults
# per preset (override with GENGIN_<PRESET>_MODEL or a bare model argument).
if [[ -n "$PRESET" && ${#MODEL_ARGS[@]} -eq 0 ]]; then
  case "$PRESET" in
    local)      MODEL_ARGS=(-m "${GENGIN_LOCAL_MODEL:-Qwen3.8-27B}") ;;
    deepseek)   MODEL_ARGS=(-m "${GENGIN_DEEPSEEK_MODEL:-deepseek-v4-flash}") ;;
    openrouter) MODEL_ARGS=(-m "${GENGIN_OPENROUTER_MODEL:-deepseek/deepseek-v4-flash-0731}") ;;
  esac
fi

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
  echo "=== gengin optimizer (supervised) ==="
  echo "HERMES_HOME: $HERMES_HOME_RESOLVED"
  echo "model:       $MODEL"
  echo "query:       $QUERY_FILE_ARG"
  echo "usage:       $USAGE_FILE_ARG"
  echo
  cd "$LLMOPT_DIR"
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
if [[ -z "${DEEPSEEK_API_KEY:-}" ]]; then
  DEEPSEEK_VALUE="$(grep -E '^DEEPSEEK_API_KEY=' "$LLMOPT_DIR/.env" 2>/dev/null | head -1 | cut -d= -f2- || true)"
  if [[ -n "$DEEPSEEK_VALUE" ]]; then
    export DEEPSEEK_API_KEY="$DEEPSEEK_VALUE"
  fi
fi

mkdir -p "$HERMES_DIR/cache"
QUERY_FILE="$HERMES_DIR/cache/query-$(date +%s).md"
{
  cat "$PROMPT_FILE"
  echo
  echo "---"
  if [[ -n "$GOAL" ]]; then
    echo "Session goal: $GOAL"
    echo
  fi
  echo "Start now. Work the profile -> micro-benchmark -> pre-mortem -> apply -> validate -> PR loop."
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

echo "=== gengin optimizer (Hermes) ==="
echo "HERMES_HOME: $HERMES_DIR"
echo "Query:       $QUERY_FILE"
echo
cd "$LLMOPT_DIR"
HERMES_HOME="$HERMES_DIR" exec "${CMD[@]}" "${PROVIDER_ARGS[@]}" "${MODEL_ARGS[@]}"
