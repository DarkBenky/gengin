#!/usr/bin/env bash
# Prepare the project-scoped Hermes Agent config for the gengin optimizer.
# Idempotent — re-run after template changes.
#
# Renders llmOpt/hermes/config.yaml.template into llmOpt/.hermes/config.yaml and
# mirrors the API keys into llmOpt/.hermes/.env.  HERMES_HOME is pointed at
# llmOpt/.hermes by scripts/gengin-opt.sh, so the global ~/.hermes config is
# never touched.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLMOPT_DIR="$(dirname "$SCRIPT_DIR")"
HERMES_DIR="$LLMOPT_DIR/.hermes"
TEMPLATE="$LLMOPT_DIR/hermes/config.yaml.template"
SECRET_SRC="$LLMOPT_DIR/.env"

if [[ ! -f "$TEMPLATE" ]]; then
  echo "error: missing $TEMPLATE" >&2
  exit 1
fi

mkdir -p "$HERMES_DIR"

PYTHON_BIN="$(command -v python3 || true)"
LOCAL_MODEL="${GENGIN_LOCAL_MODEL:-Qwen3.8-27B}"
# Bench runs need an X display; :2 is this machine's headless Xorg (auth-free).
DISPLAY_VALUE="${GENGIN_DISPLAY:-:2}"

sed -e "s|__LLMOPT_DIR__|$LLMOPT_DIR|g" \
    -e "s|__PYTHON__|${PYTHON_BIN:-python3}|g" \
    -e "s|__LOCAL_MODEL__|$LOCAL_MODEL|g" \
    -e "s|__DISPLAY__|$DISPLAY_VALUE|g" \
    "$TEMPLATE" > "$HERMES_DIR/config.yaml"

# HERMES_HOME/.env is the profile's secret file.  KEY is the raw OpenRouter key
# name in llmOpt/.env; DEEPSEEK_API_KEY enables the direct DeepSeek provider;
# GITHUB_TOKEN is used by the create_pr tool.
if [[ -f "$SECRET_SRC" ]]; then
  KEY_VALUE="$(grep -E '^KEY=' "$SECRET_SRC" | head -1 | cut -d= -f2- || true)"
  DEEPSEEK_VALUE="$(grep -E '^DEEPSEEK_API_KEY=' "$SECRET_SRC" | head -1 | cut -d= -f2- || true)"
  GITHUB_VALUE="$(grep -E '^GITHUB_TOKEN=' "$SECRET_SRC" | head -1 | cut -d= -f2- || true)"
  if [[ -n "$KEY_VALUE" || -n "$DEEPSEEK_VALUE" ]]; then
    umask 077
    {
      if [[ -n "$KEY_VALUE" ]]; then
        printf 'OPENROUTER_API_KEY=%s\n' "$KEY_VALUE"
      fi
      if [[ -n "$DEEPSEEK_VALUE" ]]; then
        printf 'DEEPSEEK_API_KEY=%s\n' "$DEEPSEEK_VALUE"
      fi
      if [[ -n "$GITHUB_VALUE" ]]; then
        printf 'GITHUB_TOKEN=%s\n' "$GITHUB_VALUE"
      fi
    } > "$HERMES_DIR/.env"
  else
    echo "warning: no KEY/DEEPSEEK_API_KEY in $SECRET_SRC — $HERMES_DIR/.env not written" >&2
  fi
else
  echo "warning: $SECRET_SRC not found — $HERMES_DIR/.env not written" >&2
fi

echo "wrote $HERMES_DIR/config.yaml"
if [[ -f "$HERMES_DIR/.env" ]]; then
  echo "wrote $HERMES_DIR/.env"
fi

# The MCP server runs on this interpreter — verify its dependency is present.
if [[ -n "$PYTHON_BIN" ]] && ! "$PYTHON_BIN" -c 'import mcp' >/dev/null 2>&1; then
  echo "warning: 'mcp' package missing for $PYTHON_BIN" >&2
  echo "  install: $PYTHON_BIN -m pip install -r $LLMOPT_DIR/requirements-mcp.txt" >&2
fi
if [[ -n "$PYTHON_BIN" ]] && ! "$PYTHON_BIN" -c 'import numpy' >/dev/null 2>&1; then
  echo "warning: 'numpy' missing for $PYTHON_BIN (image comparison tools)" >&2
  echo "  install: $PYTHON_BIN -m pip install -r $LLMOPT_DIR/requirements-mcp.txt" >&2
fi

if command -v hermes >/dev/null 2>&1; then
  if HERMES_HOME="$HERMES_DIR" hermes config get model.default >/dev/null 2>&1; then
    echo "hermes config check: OK"
  else
    echo "warning: 'hermes config get model.default' failed — run 'hermes doctor'" >&2
  fi
  echo "MCP check: HERMES_HOME=$HERMES_DIR hermes mcp test gengin"
else
  echo
  echo "hermes is not installed. Install it with:"
  echo "  curl -fsSL https://raw.githubusercontent.com/NousResearch/hermes-agent/main/scripts/install.sh | bash"
fi

echo
echo "run a session: $SCRIPT_DIR/gengin-opt.sh [local|openrouter|deepseek] [model] [--headless]"
