# llmOpt — MCP tool server for the gengin optimizer

Build / bench / profile / PR tools for the `gengin` CPU ray tracer, exposed over
MCP (stdio) and driven by an agent harness — [DeepSeek Harness
(dsh)](https://github.com/deepseek-ai/deepseek-harness) by default, or the
[Hermes Agent](https://hermes-agent.nousresearch.com/docs). The harness edits
and searches; this server owns the pipeline (sandbox, make/flame/bench,
micro-bench, perf, bisection, clangd).

## Setup

    pip install -r llmOpt/requirements-mcp.txt
    llmOpt/scripts/setup-dsh.sh           # renders llmOpt/.dsh/profiles/headless/cordis.patch.yml (default harness)
    llmOpt/scripts/setup-hermes.sh        # optional: renders llmOpt/.hermes/{config.yaml,.env} (Hermes harness)

Project-scoped only — the global `~/.hermes` / `~/.dsh` configs are never
touched. dsh itself: `npm install -g @deepseek-ai/dsh` (Node 18+, developer
preview). Hermes itself: `curl -fsSL https://raw.githubusercontent.com/NousResearch/hermes-agent/main/scripts/install.sh | bash`.

## Launch

    llmOpt/scripts/gengin-opt.sh                       # default model (OpenRouter flash)
    llmOpt/scripts/gengin-opt.sh local                 # local model; dsh TUI (interactive terminal UI)
    llmOpt/scripts/gengin-opt.sh local --web           # dsh Web UI instead (browser; :3081)
    llmOpt/scripts/gengin-opt.sh local --headless      # auto-run one-shot, no UI
    llmOpt/scripts/gengin-opt.sh local --hermes        # Hermes TUI instead
    llmOpt/scripts/gengin-opt.sh local Qwen3.8-27B --goal "speed up rayTriangle"
    llmOpt/scripts/gengin-opt.sh deepseek              # direct DeepSeek API
    llmOpt/scripts/gengin-opt.sh openrouter deepseek/deepseek-v4-pro
    llmOpt/scripts/gengin-opt.sh --headless            # unattended oneshot (-z)
    llmOpt/scripts/gengin-opt.sh --harness hermes      # same session on Hermes Agent (shorthand: --hermes)
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

## DeepSeek Harness (dsh) — default harness

`dsh` runs the same MCP tool server and the same prompt through
[DeepSeek Harness](https://github.com/deepseek-ai/deepseek-harness). It is the
default for both the launcher and the supervisor; `--hermes` (or
`--harness hermes` / `GENGIN_HARNESS=hermes`) selects the legacy Hermes Agent
instead — `gengin-opt.sh local --hermes` runs the local preset on Hermes — and
`--dsh` forces dsh back for a single run. Everything around the session
(sandbox, proxy, budget cap, PR gates) is unchanged; only the agent process
differs. dsh reads the model and endpoint from a rendered profile patch layer
under `DSH_HOME`; the default (`profiles/headless`, from `scripts/setup-dsh.sh`)
routes through the OpenRouter proxy, so `--provider`/`-m` are ignored in this
mode — set `GENGIN_OPENROUTER_MODEL` and re-run `setup-dsh.sh` to change the
model. The `local` preset renders its own dsh profiles against the local
router instead (`LOCAL_LLM_BASE`, default `http://localhost:8013/v1`), keeping
the full MCP toolset. An interactive `local` run boots the dsh TUI
([dsh-TUI](https://github.com/ccch1mneyyy/dsh-TUI), a third-party terminal UI
plugin for dsh; primary target dsh 0.2.0-rc.2): one-time install is
`npm install -g @deepseek-harness-tui/dsh-tui` plus
`DSH_HOME=llmOpt/.dsh dsh plugin --profile dsh-tui add
@deepseek-harness-tui/dsh-tui`; the launcher then boots the TUI via the
`dsh-tui` launcher itself (it tags the launch so the plugin mounts; a plain
`dsh --profile dsh-tui` silently skips the frontend when stdout is not a TTY)
with a regenerated `--patch` overlay (same provider/MCP wiring, provider id
`local-router`) and passes the session brief as the first prompt. Interactive
runs re-attach stdout to `/dev/tty` first, so shell capture hooks (e.g. a zsh
preexec doing `exec > >(tee log)`) cannot blank the UI. The TUI keeps
dsh's session store, so its sessions resume in the TUI and also show up in the
Web UI. First launch shows the TUI's own setup wizard (language/theme/workspace);
the model is pre-wired by the overlay. `local --web` boots the steerable Web UI
instead (`profiles/local-web`): the launcher prints a bookmark URL for
`http://127.0.0.1:3081` (`GENGIN_WEB_PORT` overrides) and never opens a browser;
if a host is already running on that port it prints the bookmark and exits.
`local --headless` keeps the auto-run one-shot (`profiles/local`); a running
one-shot cannot be steered (its process owns the session, it is only visible in
a web UI's history). The terminal owns the TUI/Web-UI process: exit stops it,
sessions persist and are resumable from the next run. The proxy still sits in
front of OpenRouter for the headless route, so quantization, routing and budget
keep working there.

Two-user VM deployments (`AGENT_USER` set) run either harness: the root-owned
`agent-launch` helper is harness-aware, and `setup-vm.sh` installs Node.js and
`@deepseek-ai/dsh` system-wide so the agent user can launch dsh. Pin
`GENGIN_HARNESS=hermes` to keep Hermes instead.

Verified against dsh 0.2.x (developer preview — breaking changes are promised):

- dsh composes config from shipped bundle layers plus a user `cordis.patch.yml`
  patch (a row addresses a shipped row by id and replaces its whole `config`, or
  inserts new rows). The provider lives under entry `llm-pi-ai` →
  `config.providers.gengin-proxy` (`apiKeyEnv`, `api: openai-completions`,
  `baseURL`, `compat`, `models`); the default model is pinned by
  `agent-default-model`. Re-check these ids/fields in
  `dsh/headless.cordis.patch.yml.template` after any dsh upgrade.
- The gengin MCP server is one `insert` row (`mcp-gengin`,
  `@deepseek-ai/dsh-mcp-client`, stdio). The bridge ships inside the dsh CLI, so
  no `dsh plugin add` is needed. It bridges MCP Tools only (Resources/Prompts
  deferred) and its discovery timeout is 60 s (not configurable).
- Interactive surfaces (Web UI, dsh TUI) pin the provider model id
  `local-router`: the router ignores ids (display-only), but dsh sessions
  persist a per-session selection, and the router's reported id flips between
  backend modes (fast 7B gguf path vs big-model id) - a stale saved id fails
  prompts with "provider 'local-gengin' has no configured model" until picked
  again.
- The bridge scrubs credential-named and `DSH_*` variables from the MCP child's
  inherited environment, so `OPENROUTER_API_KEY`/`GITHUB_TOKEN` are re-declared
  in the row's `env` as `!!js process.env.X` expressions (evaluated in the dsh
  host at spawn). `GENGIN_*` session vars are not scrubbed and ride the
  inherited environment.
- Proprietary code opt-out: the session-log-to-DeepSeek row is `disabled: true`
  in the patch; the launcher-owned OTel row is disabled by exporting
  `DSH_TELEMETRY_MODE=DISABLED DSH_TELEMETRY_DISABLED=1` (config cannot disable
  that row).
- dsh sandboxes shell commands with bubblewrap by default (no `/dev/nvidia*`,
  no X sockets - the GPU benches fail inside it): `gengin-opt.sh` exports
  `DSH_PERMISSION_MODE=danger-full-access` for every dsh run (manual and
  supervised), restoring the full host access the Hermes sessions had. Export
  the variable yourself when invoking `dsh` directly.
- dsh has no `--usage-file`; spend is tracked through the capped OpenRouter key
  and the proxy cost log.

**Local browser UI (desktop).** `scripts/dsh-web-local.sh` serves the dsh Web
UI at `http://127.0.0.1:3080` against the local model server (default
`http://localhost:8013/v1`, the local router) - model-only: no MCP tools, no
workspace wiring, no session-log/OTel export. The optimizer's own per-run UI
(:3081, see the `local` preset above) is separate; both share the pinned login
token (`scripts/dsh-web-common.sh`). `systemd/dsh-web-local.service`
runs it at login (ordered after the router, retrying until `/v1/models`
answers); `DSH_WEB_WAIT_SECONDS` controls the retry window:

    cp llmOpt/systemd/dsh-web-local.service ~/.config/systemd/user/
    systemctl --user daemon-reload
    systemctl --user enable --now dsh-web-local.service
    journalctl --user -u dsh-web-local -n 20   # prints the URL + its per-run token

## Changing things

| Want to change       | Where                                                        |
|----------------------|--------------------------------------------------------------|
| API keys (local)     | `llmOpt/.env` (`KEY`, `GITHUB_TOKEN`, `DEEPSEEK_API_KEY`), then `setup-hermes.sh` |
| API keys (VM)        | `/etc/gengin-llmopt/secrets.env` (root-only, the only place), then restart |
| Session behaviour    | `prompts/optimize.md` / `prompts/optimize-ml.md` (read per session) |
| Tuning / model (VM)  | `llmOpt/.env` from `.env.example`; model via `scripts/set-openrouter-model.sh <id>` |
| Hermes config        | `hermes/config.yaml.template` + `setup-hermes.sh`            |
| dsh config           | `dsh/headless.cordis.patch.yml.template` + `setup-dsh.sh`    |
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

| File                                                                                                    | Purpose                                                                                                          |
|---------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------|
| `mcp_server.py` / `main.py`                                                                             | MCP server (stdio) / build-bench-flame-PR-bisect logic                                                           |
| `supervisor.py`                                                                                         | Unattended commit-triggered supervisor (VM)                                                                      |
| `getFunc.py`, `perf.py`, `lsp_client.py`, `gen_compile_commands.py`                                     | C index + perf annotation, perf parsing, clangd client, compile DB                                               |
| `ml_bench.py`, `ml_baseline.json`                                                                       | Layer suites + PyTorch gate + verdicts, pinned timings                                                           |
| `scripts/gengin-opt.sh`                                                                                 | Session launcher (`--harness hermes\|dsh`; `local` = dsh TUI, `--web` = Web UI, `--headless` = auto-run one-shot) |
| `scripts/dsh-web-local.sh`, `dsh-web-common.sh`                                                         | Model-only web chat UI (:3080) + shared stable-token helper for both web hosts                                   |
| `scripts/setup-hermes.sh`, `setup-dsh.sh`, `setup-vm.sh`, `setup-openrouter-proxy.sh`, `enable-perf.sh` | One-time setup                                                                                                   |
| `scripts/supervisor-console.sh`                                                                         | Supervisor in a tmux console (VM)                                                                                |
| `codebase_context.md`                                                                                   | Architecture, wins, failures, hotspots (gitignored, per checkout)                                                |
| `general_improvements.md`                                                                               | Curated minimal-chore queue for the sessions' General Improvements fallback (tracked, shared)                    |

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
one agent session under Xvfb, and deletes the key on every exit path. One session
at a time; new commits coalesce to the newest. `GENGIN_OFFPEAK_WAIT=1` in
`llmOpt/.env` holds new commits until DeepSeek's off-peak (half-price) window.
The harness is `dsh` by default; `GENGIN_HARNESS=hermes` switches to Hermes.
Two-user deployments (`AGENT_USER` set) support both harnesses - the launch
helper is harness-aware (see the dsh section above).

    git clone git@github.com:DarkBenky/gengin.git <checkout>
    sudo bash <checkout>/llmOpt/scripts/setup-vm.sh --checkout <checkout>   # Ubuntu 22.04/24.04
    sudo nano /etc/gengin-llmopt/secrets.env
    sudo systemctl enable --now gengin-xvfb.service gengin-llmopt.service
    journalctl -u gengin-llmopt.service -f
    # or in tmux instead: <checkout>/llmOpt/scripts/supervisor-console.sh

In the console, one Ctrl-C cancels the running session (its summary records
`interrupted`) and the supervisor keeps polling; a second Ctrl-C stops it.

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
