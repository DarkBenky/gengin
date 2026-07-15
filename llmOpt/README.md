# llmOpt — LLM-driven C Renderer Optimizer

MCP server exposing 60 tools (46 domain + 14 LSP) for profiling, analyzing, and
optimizing the gengin CPU ray tracer. Driven by an external agent (OpenCode).

## Quick start

```bash
# 1. Install
npm install -g opencode-ai
pip install -r llmOpt/requirements-mcp.txt

# 2. Set API key (in llmOpt/.env)

# 3. Run the full optimization pipeline — NO PROMPT NEEDED
./llmOpt/scripts/optimize.sh          # deepseek-v4-pro (default)
./llmOpt/scripts/optimize.sh flash    # deepseek-v4-flash (cheaper)

# 4. Or provide your own prompt
cd /home/user/Desktop/gengin
opencode run --model openrouter/deepseek/deepseek-v4-pro --auto \
  --file llmOpt/prompts/optimize.md \
  "Profile with make_flame, then optimize the hottest function."
```

## Zero-prompt optimization

The script `llmOpt/scripts/optimize.sh` loads the full ISOLATION-FIRST workflow
from `llmOpt/prompts/optimize.md` and runs it automatically. The pipeline:
profile → micro-benchmark → pre-mortem → apply → validate → PR.

```bash
# Just run it — the prompt is built-in
./llmOpt/scripts/optimize.sh
```

## Common commands

```bash
# Analysis only (no edits)
opencode run --model openrouter/deepseek/deepseek-v4-flash \
  "Call get_tree, get_todos, and list_functions. Report top 3 bottlenecks."

# Switch model mid-session
# Agent calls: set_model_flash() or set_model_pro()

# Check current config
# Agent calls: get_model_config()
```

## Files

| File | Purpose |
|---|---|
| `mcp_server.py` | MCP server — 60 tools via stdio transport |
| `main.py` | Build/bench/profile domain logic |
| `getFunc.py` | C codebase indexer and editing tools |
| `lsp_client.py` | clangd LSP client — semantic code intelligence |
| `gen_compile_commands.py` | Auto-generates compile_commands.json for clangd |
| `model_config.py` | Model switcher (flash ↔ pro) |
| `perf.py` | perf.data → flamegraph parser |
| `prompts/optimize.md` | System prompt — full optimization workflow |
| `scripts/optimize.sh` | Zero-prompt wrapper — runs the full pipeline |
| `planner.py` | Task/note board (deprecated — OpenCode manages its own) |
| `executor.py` | Old command dispatcher (deprecated) |
| `modelSelector.py` | Old model router (deprecated) |
| `refine.py` | Old refinement engine (deprecated) |
| `MCP_SETUP.md` | Detailed setup + opencode.json config |
| `requirements-mcp.txt` | Python deps (`mcp>=1.27,<2`) |

## Sandbox

All tools operate on `llmOpt/gengin/` — never touches the repo root.
Your working copy is safe. Call `git_pull_project` to refresh the sandbox.
