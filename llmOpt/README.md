# llmOpt — MCP tool server for the gengin optimizer

Build / bench / profile / PR tools for the `gengin` CPU ray tracer, exposed over
MCP (stdio) and driven by the [Hermes Agent](https://hermes-agent.nousresearch.com/docs).
Hermes edits and searches; this server owns the pipeline (sandbox, make/flame/bench,
micro-bench, perf, bisection, clangd).

## Setup

    pip install -r llmOpt/requirements-mcp.txt
    llmOpt/scripts/setup-hermes.sh        # renders llmOpt/.hermes/{config.yaml,.env} from secrets

Project-scoped only — the global `~/.hermes` config is never touched.
Hermes itself: `curl -fsSL https://raw.githubusercontent.com/NousResearch/hermes-agent/main/scripts/install.sh | bash`.

## Launch

    llmOpt/scripts/gengin-opt.sh                       # default model (OpenRouter flash)
    llmOpt/scripts/gengin-opt.sh local                 # local llama.cpp on :8013
    llmOpt/scripts/gengin-opt.sh local Qwen3.8-27B --goal "speed up rayTriangle"
    llmOpt/scripts/gengin-opt.sh deepseek              # direct DeepSeek API
    llmOpt/scripts/gengin-opt.sh openrouter deepseek/deepseek-v4-pro
    llmOpt/scripts/gengin-opt.sh --headless            # unattended oneshot (-z)
    llmOpt/scripts/gengin-opt.sh ml [provider]         # ML/OpenCL-kernel objective (needs a GPU)

Provider words (`local` / `deepseek` / `openrouter <id>`) work in any order and
default to the local server. The session prompt is `prompts/optimize.md`
(`prompts/optimize-ml.md` in `ml` mode) — edit those to change what a session does.
Switch model mid-session with `/model custom:local:Qwen3.8-27B`.

`ml` extras: needs a local model with >=64K context, and a sandbox that contains
`machineLearning/bench/` (refresh with `git_pull_project`). The kernel bench pins
the fastest free GPU (3090 first) and checks output against a PyTorch oracle before
reporting speed — a wrong kernel fails, it is never "fast".

    python3 llmOpt/ml_bench.py --list | --suite core [--capture] [--configs conv:w28_h28_c1_k3_n16] [--kind conv]
    python3 machineLearning/generateKernel.py conv 28 28 1 3 16   # also pool/dense/softmax/pixelshuffle/bilinear

Suites: `smoke core stress edges chain upscale srnet all`. Baselines in
`llmOpt/ml_baseline.json` (gitignored, per suite hash + GPU; capture on a clean tree).

## Changing things

| Want to change       | Where                                                        |
|----------------------|--------------------------------------------------------------|
| API keys (local)     | `llmOpt/.env` (`KEY`, `GITHUB_TOKEN`, `DEEPSEEK_API_KEY`), then `setup-hermes.sh` |
| API keys (VM)        | `/etc/gengin-llmopt/secrets.env` (root-only, the only place), then restart |
| Session behaviour    | `prompts/optimize.md` / `prompts/optimize-ml.md` (read per session) |
| Tuning / model (VM)  | `llmOpt/.env` from `.env.example`; model via `scripts/set-openrouter-model.sh <id>` |
| Hermes config        | `hermes/config.yaml.template` + `setup-hermes.sh`            |
| Display for benches  | `GENGIN_DISPLAY=:2 llmOpt/scripts/setup-hermes.sh`           |

Secrets never live in the checkout. Never put the OpenRouter *management* key in
`llmOpt/.env` on the VM — the agent could mint unlimited keys.

## Tools (31)

| Group              | Tools |
|--------------------|-------|
| Build & profiling  | `git_pull_project`, `build_project`, `make_bench`, `make_flame`, `create_pr`, `bisect_regression` |
| Flight controller  | `flight_bench`, `flight_scenarios`, `flight_trace` |
| ML layer kernels   | `ml_bench`, `ml_scenarios`, `ml_trace`, `ml_parity` |
| Micro-bench        | `create_func_bench`, `run_func_bench`, `run_perf_stat`, `delete_func_bench` |
| Visual evidence    | `compare_bench_frames`, `compare_images` |
| Pull requests      | `list_pull_requests`, `fetch_pull_request`, `close_pull_request`, `label_pull_request` |
| Hotspots           | `hot_annotate_func`, `hot_annotate_file` |
| clangd             | `lsp_definition`, `lsp_references`, `lsp_call_hierarchy`, `lsp_diagnostics`, `lsp_diagnostics_all` |
| Session control    | `report_session_result` (supervised only) |

Visual changes are opt-in: `make_bench(allow_visual_change=true)` (SSIM gate) ->
`compare_bench_frames` (before/after/diff in `screenshots/visual/`) ->
`create_pr(imageOutputChange=true)` opens a `[visual]` PR (min SSIM 0.95).
Every PR carries exactly one category label (`create_pr(label=...)`,
`label_pull_request`); categories and their gates are defined in
`prompts/optimize.md` (PR CATEGORIES AND LABELS). With 4+ open PRs sharing one
label a session verifies them and folds survivors into one consolidation PR —
never across categories. Requires GITHUB_TOKEN with `Issues: write` for labels.

## Files

| File | Purpose |
|------|---------|
| `mcp_server.py` / `main.py` | MCP server (stdio) / build-bench-flame-PR-bisect logic |
| `supervisor.py` | Unattended commit-triggered supervisor (VM) |
| `getFunc.py`, `perf.py`, `lsp_client.py`, `gen_compile_commands.py` | C index + perf annotation, perf parsing, clangd client, compile DB |
| `ml_bench.py`, `ml_baseline.json` | Layer suites + PyTorch gate + verdicts, pinned timings |
| `scripts/gengin-opt.sh` | Session launcher |
| `scripts/setup-hermes.sh`, `setup-vm.sh`, `setup-openrouter-proxy.sh`, `enable-perf.sh` | One-time setup |
| `scripts/supervisor-console.sh` | Supervisor in a tmux console (VM) |
| `codebase_context.md` | Architecture, wins, failures, hotspots (gitignored, per checkout) |

## Sandbox

All tools work on `llmOpt/gengin/`; the repo root is untouched until
`create_pr`. `git_pull_project` refreshes it (clone + rsync of gitignored
`deps/`, `assets/`, `.flamegraph/`, `default.profdata`).

Benches open a MiniFB window, so the server needs a display (`DISPLAY=:2` here).
`run_perf_stat` and `make_flame` need unprivileged perf counters — run
`scripts/enable-perf.sh` once. GPU work auto-pins the fastest free card;
`GENGIN_NO_CLOUDS=1` skips the OpenCL cloud pass if it fails.

## Unattended supervisor (VM)

Watches `origin/main`; on a new commit it prepares an exact-SHA sandbox, runs
preflight + clean baseline, mints a budget-capped temporary OpenRouter key, runs
one Hermes session under Xvfb, and deletes the key on every exit path. One session
at a time; new commits coalesce to the newest. `GENGIN_OFFPEAK_WAIT=1` in
`llmOpt/.env` holds new commits until DeepSeek's off-peak (half-price) window.

    git clone git@github.com:DarkBenky/gengin.git <checkout>
    sudo bash <checkout>/llmOpt/scripts/setup-vm.sh --checkout <checkout>   # Ubuntu 22.04/24.04
    sudo nano /etc/gengin-llmopt/secrets.env
    sudo systemctl enable --now gengin-xvfb.service gengin-llmopt.service
    journalctl -u gengin-llmopt.service -f
    # or in tmux instead: <checkout>/llmOpt/scripts/supervisor-console.sh

After pulling new code: `git pull` then `sudo systemctl restart gengin-llmopt`
(the MCP server and prompts are re-read per session; the restart is for
`supervisor.py` itself).

    python3 llmOpt/supervisor.py --status | --dry-run | --preflight | --once | --cleanup-stale-keys

Exit codes: 0 ok, 1 runtime failure, 2 bad config, 3 corrupt state. State in
`llmOpt/state/supervisor.json`, session logs in `llmOpt/logs/sessions/`, per-session
artifacts in `llmOpt/run/<sessionId>/`. Interrupted sessions are validated and
cleaned up at startup; a corrupt state file is quarantined and stops the run;
`gengin-llmopt-*` keys are reconciled automatically.

Security: two Unix users (`llmopt-supervisor` holds the management key,
`llmopt-agent` holds the checkout), the supervisor runs from system python, and
inference keys are ephemeral, budget-capped and deleted on exit. `create_pr`
never force-pushes or merges and rejects secret-looking tokens or forbidden paths.

## OpenRouter proxy (optional)

`llmOpt/proxy/openrouter_proxy.py` on `127.0.0.1:8787` requests the quantizations
you want (`:floor`, no unfiltered fallbacks), and can switch a conversation to a
cheaper model once the prompt crosses a token threshold:

    llmOpt/scripts/gengin-opt.sh anthropic/claude-haiku-5.5:anthropic --max-context 99K deepseek

`--budget 15` caps session spend (warns the model at 80%/95%, HTTP 402 past it);
`--wait-offpeak` waits for DeepSeek's half-price window. Every request is logged to
SQLite and shown at `http://127.0.0.1:8787/logs` (per-interval chart, cache-hit %,
CSV/JSON export). Desktop:
`scripts/setup-openrouter-proxy.sh`; on the VM it starts with the supervisor.
Inspect with `curl 127.0.0.1:8787/status` or `/route`. Knobs are `GENGIN_PROXY_*`
and `GENGIN_ROUTE_*` in `llmOpt/.env`.
