#!/bin/bash
# Zero-prompt optimization loop — runs the full pipeline automatically.
#
# Usage:
#   ./llmOpt/scripts/optimize.sh          # v4-pro (default)
#   ./llmOpt/scripts/optimize.sh flash    # v4-flash (cheaper)
#   ./llmOpt/scripts/optimize.sh pro      # v4-pro (explicit)
#
# Prerequisites:
#   npm install -g opencode-ai
#   pip install -r llmOpt/requirements-mcp.txt
#   API key in llmOpt/.env (KEY=sk-or-v1-...)

set -euo pipefail

MODEL="${1:-pro}"
case "$MODEL" in
    flash) MODEL_ID="openrouter/deepseek/deepseek-v4-flash" ;;
    pro)   MODEL_ID="openrouter/deepseek/deepseek-v4-pro" ;;
    *)
        echo "Usage: $0 [flash|pro]"
        echo "  flash — deepseek-v4-flash (cheaper, faster)"
        echo "  pro   — deepseek-v4-pro   (higher quality, default)"
        exit 1
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
PROMPT_FILE="$SCRIPT_DIR/../prompts/optimize.md"

if [ ! -f "$PROMPT_FILE" ]; then
    echo "Error: prompt file not found: $PROMPT_FILE"
    exit 1
fi

# Load API key from .env
if [ -f "$SCRIPT_DIR/../.env" ]; then
    export OPENROUTER_API_KEY=$(grep '^KEY=' "$SCRIPT_DIR/../.env" | cut -d= -f2)
fi

if [ -z "${OPENROUTER_API_KEY:-}" ]; then
    echo "Error: OPENROUTER_API_KEY not set. Add KEY=sk-or-v1-... to llmOpt/.env"
    exit 1
fi

cd "$PROJECT_DIR"

echo "=== gengin optimizer ==="
echo "Model:  $MODEL_ID"
echo "Prompt: $PROMPT_FILE"
echo "Project: $PROJECT_DIR"
echo

exec opencode run \
    --model "$MODEL_ID" \
    --auto \
    --file "$PROMPT_FILE" \
    "Follow the ISOLATION-FIRST workflow in the attached file.

Begin by assessing the current state:
1. Call get_codebase_context to load accumulated insights from prior sessions.
2. Call get_diff to check for uncommitted changes in the sandbox.
3. Call get_tree to see the project layout.
4. If already profiled: call lsp_show_context on the top hotspot.
5. If not profiled: call build_project, then make_flame to find hotspots.
6. Then systematically optimize each hot function using the micro-benchmark
   pipeline — prefer LSP tools over regex tools for all navigation and editing.

Start now."
