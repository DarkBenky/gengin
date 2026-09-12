# llmOpt — MCP tool server for the gengin optimizer

Deterministic build/bench/profile/PR tools for the `gengin` CPU ray tracer,
exposed over MCP (stdio) and driven by the [Hermes Agent](https://hermes-agent.nousresearch.com/docs)
harness.  Editing, searching, and terminal work are done by Hermes's built-in
tools; this server owns the domain pipeline: sandbox lifecycle, make/flame/bench,
micro-benchmark sandbox, perf annotation, regression bisection, and clangd queries.

## Setup

1. Install Hermes Agent (once):

       curl -fsSL https://raw.githubusercontent.com/NousResearch/hermes-agent/main/scripts/install.sh | bash

2. Install the MCP dependency:

       pip install -r llmOpt/requirements-mcp.txt

3. Render the project-scoped config:

       llmOpt/scripts/setup-hermes.sh

   This writes `llmOpt/.hermes/config.yaml` and `llmOpt/.hermes/.env` and points
   `HERMES_HOME` there — the global `~/.hermes` config is never touched.

## Run

    llmOpt/scripts/gengin-opt.sh                  # default model (OpenRouter flash)
    llmOpt/scripts/gengin-opt.sh openrouter deepseek/deepseek-v4-pro
    llmOpt/scripts/gengin-opt.sh local            # local llama.cpp server on :8012
    llmOpt/scripts/gengin-opt.sh local Qwen3.8-27B --goal "speed up rayTriangle"
    llmOpt/scripts/gengin-opt.sh deepseek         # direct DeepSeek API (DEEPSEEK_API_KEY)
    llmOpt/scripts/gengin-opt.sh --headless       # unattended: scripted oneshot (-z), no approvals

The launcher loads `prompts/optimize.md` as the session query.  In-session you
can switch models with `/model custom:local:Qwen3.8-27B` or
`/model openrouter:deepseek/deepseek-v4-flash-0731`.

## Tools (17)

| Group | Tools |
|---|---|
| Build & profiling | `git_pull_project`, `build_project`, `make_bench`, `make_flame`, `create_pr`, `bisect_regression` |
| Micro-bench sandbox | `create_func_bench`, `run_func_bench`, `run_perf_stat`, `delete_func_bench` |
| Hotspot annotation | `hot_annotate_func`, `hot_annotate_file` |
| clangd queries | `lsp_definition`, `lsp_references`, `lsp_call_hierarchy`, `lsp_diagnostics`, `lsp_diagnostics_all` |

## Files

| File | Purpose |
|---|---|
| `mcp_server.py` | MCP server (stdio) — the 17 domain tools |
| `main.py` | Build/bench/flame/PR/bisect domain logic |
| `getFunc.py` | C function/struct index + perf line annotation |
| `lsp_client.py` | clangd client (definition/references/diagnostics/call hierarchy) |
| `gen_compile_commands.py` | Generates compile_commands.json for clangd |
| `perf.py` | perf.data → folded stacks → hotspot parser |
| `prompts/optimize.md` | Session workflow (ISOLATION-FIRST loop) |
| `hermes/config.yaml.template` | Project Hermes config template |
| `scripts/setup-hermes.sh` | Renders the template + secrets into `llmOpt/.hermes/` |
| `scripts/gengin-opt.sh` | Session launcher with model selection |
| `codebase_context.md` | Persisted insights: architecture, wins, failures, hotspots |

## Sandbox

All tools operate on `llmOpt/gengin/` — the repo root is never touched until
`create_pr`.  `git_pull_project` refreshes the sandbox (clone + rsync of the
gitignored `deps/`, `assets/`, `.flamegraph/`, `default.profdata`).  Secrets
live in `llmOpt/.env` (`KEY`, `GITHUB_TOKEN`) and are mirrored into
`llmOpt/.hermes/.env` by the setup script.

Benches open a MiniFB window, so the MCP server needs a display: the rendered
config passes `DISPLAY=:2` (this machine's headless Xorg).  Override with
`GENGIN_DISPLAY=<display> llmOpt/scripts/setup-hermes.sh`.  `run_perf_stat` and
`make_flame` need unprivileged perf counters — `llmOpt/scripts/enable-perf.sh`
enables them once (sudo prompt; persists via `/etc/sysctl.d`, with a
`cap_perfmon` fallback).
