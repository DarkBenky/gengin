# MCP Server Setup — gengin-optimizer

How to run the `llmOpt/` domain tools as an MCP server and connect an external
agent harness (OpenCode) to drive C codebase performance optimization.

## Prerequisites

- Python 3.10 or later
- The gengin C project checked out at `gengin/` under the repo root (or use
  `git_pull_project` tool to clone it)
- `perf` installed (`sudo apt install linux-tools-common linux-tools-generic`)
  for flamegraph / perf-stat tools

## Install

```bash
pip install -r llmOpt/requirements-mcp.txt
```

This installs only `mcp>=1.27,<2` — the official Python MCP SDK.

## Run the server

```bash
cd /home/user/Desktop/gengin
python llmOpt/mcp_server.py
```

The server uses **stdio transport** (the default for MCP).  It prints startup
info to stderr and listens on stdin for JSON-RPC requests.

At startup the server:
1. Indexes all `.c`, `.h`, `.cl` files in `gengin/` (1-2 seconds)
2. Prints the `opencode.json` config snippet to stderr
3. Reports how many functions and source files were indexed
4. Waits for tool calls

The first `make_bench()` call will be slow (~30 s) because it establishes the
performance baseline.  Subsequent calls compare against that baseline.

## Connect OpenCode

Create `opencode.json` in the project root (`/home/user/Desktop/gengin/opencode.json`):

```jsonc
{
  "$schema": "https://opencode.ai/config.json",
  "model": "openrouter/deepseek/deepseek-v4-flash",
  "mcp": {
    "gengin-optimizer": {
      "type": "local",
      "command": ["python", "llmOpt/mcp_server.py"],
      "enabled": true,
      "timeout": 60000
    }
  }
}
```

Set your OpenRouter API key as an environment variable:

```bash
export OPENROUTER_API_KEY=sk-or-v1-...
```

Then run an optimization task:

```bash
opencode run --model openrouter/deepseek/deepseek-v4-flash "your prompt"
```

### Example provider blocks for other models

**Ollama (local):**

```jsonc
{
  "model": "ollama/qwen3:14b"
}
```

**OpenRouter with specific provider routing:**

```jsonc
{
  "model": "openrouter/deepseek/deepseek-v4-flash",
  "provider": {
    "openrouter": {
      "options": {
        "apiKey": "{env:OPENROUTER_API_KEY}"
      }
    }
  }
}
```

## Available tools (45 total)

The server exposes every domain tool from `main.py` and `getFunc.py`.
Use the `api_help` tool to list them all at runtime with signatures and
descriptions.  Here is a quick reference:

### Build & Profiling
| Tool | Description |
|---|---|
| `git_pull_project` | Clone gengin repo, install deps, sync assets |
| `get_tree` | Directory tree of the project |
| `get_todos` | Grep all TODO comments |
| `build_project` | Run make (with syntax check + auto-review) |
| `make_bench` | Run make bench, return comparison vs baseline |
| `make_flame` | Run make flame, return perf data + hotspots |
| `create_pr` | Commit, push, open a GitHub PR |

### Micro-Benchmark Sandbox
| Tool | Description |
|---|---|
| `create_func_bench` | Create bench/<name>.h and bench/<name>.c |
| `run_func_bench` | Build and run a micro-benchmark |
| `run_perf_stat` | Run perf stat on a micro-benchmark binary |
| `delete_func_bench` | Remove bench files for a function |

### Validation & Bisection
| Tool | Description |
|---|---|
| `review_changes` | Send git diff to reviewer model for bug check |
| `bisect_regression` | Identify which edit caused a regression |

### Knowledge Persistence
| Tool | Description |
|---|---|
| `sync_planner_to_codebase_context` | Distill planner notes into codebase_context.md |
| `get_codebase_context` | Read the persisted codebase knowledge base |

### Exploration
| Tool | Description |
|---|---|
| `show_context` | Target function + callees + used types |
| `show_context_with_meta` | Like show_context but returns structured dict |
| `get_definition` | Function or struct definition |
| `get_callers` | List all callers of a function |
| `get_diff` | Git diff vs HEAD |
| `grep_source` | grep pattern across .c/.h/.cl files |
| `find_symbol` | Word-boundary grep for an identifier |
| `list_dir` | List files/dirs under a path |
| `read_lines` | Read specific line range from a file |
| `run_command` | Run a whitelisted analysis command |
| `read_source_file` | Read raw file content |
| `list_functions` | List all indexed functions |
| `show_src` | Show file with line numbers |
| `show_src_pair` | Show .c file with its companion .h |
| `hot_annotate_func` | Annotate function source with perf percentages |
| `hot_annotate_file` | Annotate entire file with perf percentages |

### Editing
| Tool | Description |
|---|---|
| `search_replace` | Find-and-replace text in a file |
| `preview_change` | Preview what search_replace would change |
| `search_replace_multi` | Multiple find-and-replace pairs (atomic) |
| `apply_change` | Replace a named function definition in-place |
| `replace_lines` | Replace specific line range |
| `insert_lines` | Insert text after a given line |
| `delete_lines` | Delete a line range |
| `apply_patch` | Batch line-range replacement |

### Git & Restore
| Tool | Description |
|---|---|
| `restore_all` | git checkout HEAD -- . (discard all changes) |
| `restore_file` | Restore a single file to HEAD |
| `restore_function` | Restore the file containing a function |
| `api_help` | Print all API signatures and descriptions |

## Safety

- **Path traversal**: `list_dir`, `read_lines`, `grep_source`, `run_command`,
  and `read_source_file` all reject `..` in paths.
- **Command whitelist**: `run_command` only allows specific analysis/build tools.
  Arbitrary shell commands are rejected.
- **Auto-restore on visual regression**: `make_bench` automatically calls
  `restore_all` if rendered frames differ significantly from baseline (MSE >= 50).
- **Pre-build review**: `build_project` runs a fast syntax check on changed `.c`
  files and optionally sends the diff to a reviewer model before the ~30 s build.
- **Transaction-safe edits**: `search_replace_multi` validates all replacements
  before writing any — partial edits never happen.

## Deprecated files

The old custom agent orchestration layer (`planner.py`, `executor.py`,
`modelSelector.py`, `refine.py`) is still present but marked **DEPRECATED**.
These modules are only used when running the old standalone loop via
`main.py` directly.  The MCP server does not import or use them.
