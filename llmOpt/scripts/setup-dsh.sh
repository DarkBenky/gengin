#!/usr/bin/env bash
# Prepare the project-scoped DeepSeek Harness (dsh) config for the gengin
# optimizer. Idempotent — re-run after template changes.
#
# Renders llmOpt/dsh/settings.yaml.template into llmOpt/.dsh/settings.yaml.
# DSH_HOME points at llmOpt/.dsh, so the global ~/.dsh config is never touched.
# No secrets are rendered: the capped OpenRouter key arrives per session through
# the environment (supervised runs) or llmOpt/.env (manual runs export it).
#
# PILOT: dsh is a v0.1 developer preview with promised breaking changes. Pin
# the version you test against and re-check the provider/mcp field names in the
# template against that version's README before an unattended run.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLMOPT_DIR="$(dirname "$SCRIPT_DIR")"
DSH_DIR="$LLMOPT_DIR/.dsh"
TEMPLATE="$LLMOPT_DIR/dsh/settings.yaml.template"

if [[ ! -f "$TEMPLATE" ]]; then
  echo "error: missing $TEMPLATE" >&2
  exit 1
fi

mkdir -p "$DSH_DIR"

PYTHON_BIN="$(command -v python3 || true)"
# Bench runs need an X display; :2 is this machine's headless Xorg (auth-free).
DISPLAY_VALUE="${GENGIN_DISPLAY:-:2}"

sed -e "s|__LLMOPT_DIR__|$LLMOPT_DIR|g" \
    -e "s|__PYTHON__|${PYTHON_BIN:-python3}|g" \
    -e "s|__DISPLAY__|$DISPLAY_VALUE|g" \
    "$TEMPLATE" > "$DSH_DIR/settings.yaml"
chmod 600 "$DSH_DIR/settings.yaml"
echo "wrote $DSH_DIR/settings.yaml"

# dsh resolves the model and endpoint from the profile settings; the OpenRouter
# key comes from the environment at session time (never rendered into a file).

# MCP tools are off by default in dsh. Register the official client plugin for
# the headless profile; best effort and safe to re-run.
if command -v dsh >/dev/null 2>&1; then
  if DSH_HOME="$DSH_DIR" timeout 120 dsh plugin --profile headless \
      add @deepseek-ai/dsh-mcp-client >/dev/null 2>&1; then
    echo "dsh-mcp-client plugin: registered for the headless profile"
  else
    echo "warning: could not register @deepseek-ai/dsh-mcp-client (offline, or already present) — run:" >&2
    echo "  DSH_HOME=$DSH_DIR dsh plugin --profile headless add @deepseek-ai/dsh-mcp-client" >&2
  fi
else
  echo
  echo "dsh is not installed. Install it with:"
  echo "  npm install -g @deepseek-ai/dsh   # Node.js 18+, developer preview"
fi

# The MCP server runs on this interpreter — verify its dependencies are present.
if [[ -n "$PYTHON_BIN" ]] && ! "$PYTHON_BIN" -c 'import mcp' >/dev/null 2>&1; then
  echo "warning: 'mcp' package missing for $PYTHON_BIN" >&2
  echo "  install: $PYTHON_BIN -m pip install -r $LLMOPT_DIR/requirements-mcp.txt" >&2
fi
if [[ -n "$PYTHON_BIN" ]] && ! "$PYTHON_BIN" -c 'import numpy' >/dev/null 2>&1; then
  echo "warning: 'numpy' missing for $PYTHON_BIN (image comparison tools)" >&2
  echo "  install: $PYTHON_BIN -m pip install -r $LLMOPT_DIR/requirements-mcp.txt" >&2
fi

echo
echo "verify before an unattended run:"
echo "  - dsh --profile headless \"say OK\"   # boots, reaches the proxy, answers"
echo "  - confirm the gengin MCP tools are listed (dsh-mcp-client discovery has"
echo "    a fixed 60 s timeout; a cold start of mcp_server.py must beat it)"
echo "  - confirm \${VAR} expansion in the settings env block delivers"
echo "    DISPLAY and GENGIN_* to the MCP child (unverified pilot item)"
echo
echo "run a session: $SCRIPT_DIR/gengin-opt.sh --harness dsh [--headless]"
