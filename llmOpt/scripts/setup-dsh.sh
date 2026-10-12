#!/usr/bin/env bash
# Prepare the project-scoped DeepSeek Harness (dsh) config for the gengin
# optimizer. Idempotent — re-run after template changes.
#
# Renders llmOpt/dsh/headless.cordis.patch.yml.template into
# llmOpt/.dsh/profiles/headless/cordis.patch.yml (dsh's user patch layer for
# the headless profile). DSH_HOME points at llmOpt/.dsh, so the global ~/.dsh
# config is never touched.
# No secrets are rendered: the capped OpenRouter key arrives per session through
# the environment (supervised runs) or llmOpt/.env (manual runs export it).
#
# PILOT: dsh is a developer preview with promised breaking changes. Pin the
# version you test against and re-check the field names in the template
# against that version's docs before an unattended run (verified against
# 0.2.x: docs/user/guide/providers.md, packages/mcp/mcp-client/README.md).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLMOPT_DIR="$(dirname "$SCRIPT_DIR")"
DSH_DIR="$LLMOPT_DIR/.dsh"
TEMPLATE="$LLMOPT_DIR/dsh/headless.cordis.patch.yml.template"
PATCH_DIR="$DSH_DIR/profiles/headless"
PATCH_FILE="$PATCH_DIR/cordis.patch.yml"

if [[ ! -f "$TEMPLATE" ]]; then
  echo "error: missing $TEMPLATE" >&2
  exit 1
fi

mkdir -p "$PATCH_DIR"

PYTHON_BIN="$(command -v python3 || true)"
# Bench runs need an X display; :2 is this machine's headless Xorg (auth-free).
DISPLAY_VALUE="${GENGIN_DISPLAY:-:2}"
MODEL_VALUE="${GENGIN_OPENROUTER_MODEL:-deepseek/deepseek-v4-flash-0731}"

# The MCP bridge scrubs credential-named variables from the child's inherited
# environment, so the manual-run marker expansion re-declares them as host-side
# !!js expressions (the supervisor renders its own fuller set per session).
python3 - "$TEMPLATE" "$PATCH_FILE" "$LLMOPT_DIR" "${PYTHON_BIN:-python3}" \
    "$DISPLAY_VALUE" "$MODEL_VALUE" <<'PY'
import sys
template, out, llmopt_dir, python_bin, display, model = sys.argv[1:7]
text = open(template).read()
text = (text.replace("__LLMOPT_DIR__", llmopt_dir)
            .replace("__PYTHON__", python_bin)
            .replace("__DISPLAY__", display)
            .replace("__MODEL__", model))
marker = "# __SESSION_ENV__"
if marker not in text:
    sys.exit(f"error: template lacks the {marker} marker")
lines = ("OPENROUTER_API_KEY: !!js \"process.env.OPENROUTER_API_KEY ?? ''\"\n"
         "          GITHUB_TOKEN: !!js \"process.env.GITHUB_TOKEN ?? ''\"")
text = text.replace(marker, lines)
open(out, "w").write(text)
PY
chmod 600 "$PATCH_FILE"
echo "wrote $PATCH_FILE"

# @deepseek-ai/dsh-mcp-client ships inside the dsh CLI for patch layers, so no
# plugin install is needed - the template's mcp-gengin row is the wiring.

# The MCP server runs on this interpreter — verify its dependencies are present.
if [[ -n "$PYTHON_BIN" ]] && ! "$PYTHON_BIN" -c 'import mcp' >/dev/null 2>&1; then
  echo "warning: 'mcp' package missing for $PYTHON_BIN" >&2
  echo "  install: $PYTHON_BIN -m pip install -r $LLMOPT_DIR/requirements-mcp.txt" >&2
fi
if [[ -n "$PYTHON_BIN" ]] && ! "$PYTHON_BIN" -c 'import numpy' >/dev/null 2>&1; then
  echo "warning: 'numpy' missing for $PYTHON_BIN (image comparison tools)" >&2
  echo "  install: $PYTHON_BIN -m pip install -r $LLMOPT_DIR/requirements-mcp.txt" >&2
fi

if ! command -v dsh >/dev/null 2>&1; then
  echo
  echo "dsh is not installed. Install it with:"
  echo "  npm install -g @deepseek-ai/dsh   # Node.js 18+, developer preview"
else
  echo
  echo "verify the composed config without booting:"
  echo "  DSH_HOME=$DSH_DIR dsh --profile headless --dump-config | grep -E 'gengin-proxy|mcp-gengin'"
fi

echo
echo "verify before an unattended run:"
echo "  - DSH_PERMISSION_MODE=danger-full-access DSH_HOME=$DSH_DIR dsh --profile headless \"say OK\""
echo "    (dsh sandboxes shell tools with bwrap by default - no /dev, no X;"
echo "     gengin-opt.sh exports danger-full-access for session runs)"
echo "  - confirm the gengin MCP tools are listed (dsh-mcp-client discovery has"
echo "    a fixed 60 s timeout; a cold start of mcp_server.py must beat it)"
echo
echo "run a session: $SCRIPT_DIR/gengin-opt.sh --harness dsh [--headless]"
