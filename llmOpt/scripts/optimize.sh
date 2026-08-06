#!/bin/bash
# Zero-prompt optimization loop — runs the full pipeline automatically.
#
# Usage:
#   ./llmOpt/scripts/optimize.sh              # flash-0731 via OpenRouter (default)
#   ./llmOpt/scripts/optimize.sh pro          # pro via OpenRouter
#   ./llmOpt/scripts/optimize.sh flash deepseek  # flash via DeepSeek direct
#   ./llmOpt/scripts/optimize.sh pro deepseek    # pro via DeepSeek direct
#
# Prerequisites:
#   npm install -g opencode-ai
#   pip install -r llmOpt/requirements-mcp.txt
#   API key in llmOpt/.env (KEY=sk-or-v1-...)
#   For deepseek backend: DEEPSEEK_API_KEY in llmOpt/.env

set -euo pipefail

MODEL="${1:-flash}"
BACKEND="${2:-openrouter}"

case "$MODEL" in
    flash)
        case "$BACKEND" in
            openrouter) MODEL_ID="openrouter/deepseek/deepseek-v4-flash-0731" ;;
            deepseek)   MODEL_ID="deepseek/deepseek-v4-flash" ;;
            *) echo "Usage: $0 [flash|pro] [openrouter|deepseek]"; exit 1 ;;
        esac
        ;;
    pro)
        case "$BACKEND" in
            openrouter) MODEL_ID="openrouter/deepseek/deepseek-v4-pro" ;;
            deepseek)   MODEL_ID="deepseek/deepseek-v4-pro" ;;
            *) echo "Usage: $0 [flash|pro] [openrouter|deepseek]"; exit 1 ;;
        esac
        ;;
    *)
        echo "Usage: $0 [flash|pro] [openrouter|deepseek]"
        echo "  flash       -- deepseek-v4-flash (default, cheaper)"
        echo "  pro         -- deepseek-v4-pro (higher quality)"
        echo "  openrouter  -- route via OpenRouter with floor pricing (default)"
        echo "  deepseek    -- direct DeepSeek API (needs DEEPSEEK_API_KEY)"
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
    export DEEPSEEK_API_KEY=$(grep '^DEEPSEEK_API_KEY=' "$SCRIPT_DIR/../.env" | cut -d= -f2 || true)
fi

if [ -z "${OPENROUTER_API_KEY:-}" ] && [ "$BACKEND" = "openrouter" ]; then
    echo "Error: OPENROUTER_API_KEY not set. Add KEY=sk-or-v1-... to llmOpt/.env"
    exit 1
fi

cd "$PROJECT_DIR"

echo "=== gengin optimizer ==="
echo "Model:   $MODEL_ID"
echo "Backend: $BACKEND"
echo "Prompt:  $PROMPT_FILE"
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
