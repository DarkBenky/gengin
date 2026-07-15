"""
gengin-optimizer MCP Server
============================
Exposes ~45 domain tools from main.py and getFunc.py as MCP tools so an
external agent harness (OpenCode) can drive C codebase performance
optimization without the legacy custom planning/execution loop.

Tools are grouped by category:

=== Build & Profiling (main.py) ===
  git_pull_project       — Clone gengin repo, install deps, sync assets
  get_tree               — Directory tree of the project
  get_todos              — Grep all TODO comments in the project
  build_project          — Run make (with pre-build syntax check + auto-review)
  make_bench             — Run make bench, return comparison vs baseline
  make_flame             — Run make flame, return perf data + hotspots
  create_pr              — Commit, push, and open a GitHub PR

=== Micro-Benchmark Sandbox (main.py) ===
  create_func_bench      — Create bench/<name>.h and bench/<name>.c
  run_func_bench         — Build and run a micro-benchmark
  run_perf_stat          — Run perf stat on a micro-benchmark binary
  delete_func_bench      — Remove bench files for a function

=== Validation & Bisection (main.py) ===
  review_changes         — Send git diff to reviewer model for bug check
  bisect_regression      — Identify which edit caused a benchmark regression

=== Knowledge Persistence (main.py) ===
  sync_planner_to_codebase_context — Distill planner notes into codebase_context.md
  get_codebase_context    — Read the persisted codebase knowledge base

=== Exploration (getFunc.py) ===
  show_context           — Target function + callees + used types
  show_context_with_meta — Like show_context but returns structured dict
  get_definition         — Print function or struct definition
  get_callers            — List all callers of a function
  get_diff               — Show current git diff vs HEAD
  grep_source            — grep pattern across .c/.h/.cl files
  find_symbol            — Word-boundary grep for an identifier
  list_dir               — List files/dirs under a path
  read_lines             — Read specific line range from a file
  run_command            — Run a whitelisted analysis command
  read_source_file       — Read raw file content
  list_functions         — List all indexed functions
  show_src               — Show file with line numbers
  show_src_pair          — Show .c file with its companion .h
  hot_annotate_func      — Annotate function source with perf percentages
  hot_annotate_file      — Annotate entire file with perf percentages

=== Editing (getFunc.py) ===
  search_replace         — Find-and-replace text in a file
  preview_change          — Preview what search_replace would change
  search_replace_multi   — Apply multiple find-and-replace pairs atomically
  apply_change           — Replace a named function's definition in-place
  replace_lines          — Replace specific line range
  insert_lines           — Insert text after a given line
  delete_lines           — Delete a line range
  apply_patch            — Batch line-range replacement

=== Git & Restore (getFunc.py) ===
  restore_all            — git checkout HEAD -- . (discard all changes)
  restore_file           — Restore a single file to HEAD
  restore_function       — Restore the file containing a function to HEAD
  api_help               — Print all available API signatures
"""

import json
import os
import sys

# Ensure the llmOpt directory is on the path so we can import sibling modules
_llmOpt_dir = os.path.dirname(os.path.abspath(__file__))
if _llmOpt_dir not in sys.path:
    sys.path.insert(0, _llmOpt_dir)

from mcp.server.fastmcp import FastMCP

import getFunc as _gf
import main as _main

# ---------------------------------------------------------------------------
# Startup initialisation
# ---------------------------------------------------------------------------

# All tools operate on llmOpt/gengin/ — a sandboxed copy of the renderer.
# This keeps experimental changes isolated from the main repo.
_gengin_dir = os.path.join(_llmOpt_dir, "gengin")
os.makedirs(_gengin_dir, exist_ok=True)
_main.PROJECT_DIR = _gengin_dir
_gf.init(base_dir=_gengin_dir)

mcp = FastMCP("gengin-optimizer")

# Apply model configuration: default to deepseek-v4-pro via DeepSeek provider
# for all internal LLM calls (code review, research, planner sync).
import model_config as _mc
_mc._applyToMain()

# Auto-generate compile_commands.json if missing (needed for clangd LSP tools).
_cc_json = os.path.join(_gengin_dir, "compile_commands.json")
if not os.path.exists(_cc_json):
    try:
        import gen_compile_commands as _gcc
        _gcc.generate(_gengin_dir)
    except Exception as _e:
        print(f"[lsp] compile_commands.json generation skipped: {_e}", file=sys.stderr)

# ---------------------------------------------------------------------------
# LSP helpers (symbol name -> file position mapping via getFunc index)
# ---------------------------------------------------------------------------

def _symbolPosition(symbol: str, rel_path: str | None = None) -> tuple[str, int, int] | None:
    """Find a symbol and return (rel_path, line_0, char_0) for LSP queries.

    Tries clangd's documentSymbol first (exact AST positions), then falls
    back to the getFunc regex index."""
    funcs = _gf._functions
    structs = _gf._structs
    if rel_path is None:
        if symbol in funcs:
            rel_path = funcs[symbol]['file']
        elif symbol in structs:
            rel_path = structs[symbol]['file']
        else:
            return None

    # Try clangd's documentSymbol for exact AST position
    try:
        client = _getLspClient()
        syms = client.documentSymbol(rel_path)
        if syms:
            for s in syms:
                if _symName(s) == symbol:
                    rng = _symRange(s)
                    start = rng.get("start", {})
                    return (rel_path, start.get("line", 0), start.get("character", 0))
    except Exception:
        pass

    # Fallback to regex index
    if symbol in funcs:
        info = funcs[symbol]
        line0 = info['start'] - 1
        col = info.get('sig', '').find(symbol)
        return (info['file'], line0, max(col, 0))
    if symbol in structs:
        info = structs[symbol]
        line0 = info['start'] - 1
        col = info.get('full', '').find(symbol)
        return (info['file'], line0, max(col, 0))
    return None


def _fmtLocation(uri: str, rng: dict) -> str:
    """Format an LSP location as a human-readable string."""
    start = rng.get('start', {})
    end = rng.get('end', {})
    sl = start.get('line', 0) + 1
    el = end.get('line', 0) + 1
    fname = uri.replace('file://', '')
    if sl == el:
        return f"{fname}:{sl}"
    return f"{fname}:{sl}-{el}"


def _fmtSeverity(sev: int) -> str:
    if sev == 1: return "ERROR"
    if sev == 2: return "WARN"
    if sev == 3: return "INFO"
    return f"SEV{sev}"


def _getLspClient():
    """Lazily get or create the LSP client singleton."""
    import lsp_client as _lsp
    return _lsp.getClient(_gengin_dir)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _ctxCall(fn, *args, **kwargs):
    """Call fn with a local context list, return the last context message.

    Used for getFunc editing tools that return bool but store the actual
    human-readable message in the context append.  We capture that message
    so the MCP caller sees it instead of a bare True/False.
    """
    ctx: list = []
    result = fn(*args, context=ctx, **kwargs)
    if ctx:
        return ctx[-1].get("output", str(result))
    return str(result)


def _checkPath(rel_path: str) -> str | None:
    """Reject path traversal and absolute paths.  Returns an error string
    if the path is unsafe, or None if it passes the check.

    Mirrors the guards in getFunc.py (listDir, readLines, grepSource) and
    adds an absolute-path check for defense-in-depth.
    """
    normalized = rel_path.replace("\\", "/")
    if ".." in normalized.split("/"):
        return f"path traversal rejected: {rel_path!r}"
    if normalized.startswith("/"):
        return f"absolute path rejected: {rel_path!r}"
    return None


def _trimContext():
    """Truncate main.py's CONTEXT list if it grows too large, keeping the
    most recent entries so memory stays bounded in long-running sessions."""
    if len(_main.CONTEXT) > 100:
        _main.CONTEXT[:] = _main.CONTEXT[-20:]


# ===================================================================
# Build & Profiling  (main.py)
# ===================================================================

@mcp.tool()
def git_pull_project() -> str:
    """Clone the gengin repo into llmOpt/gengin/ (the sandbox directory),
    install system deps (tree), and sync assets/profdata from the parent repo.
    Destructive — removes the existing llmOpt/gengin/ first.
    After cloning, re-indexes source files so exploration tools work."""
    # git_pull_project uses subprocess.run without cwd, so it operates relative
    # to the process CWD.  We temporarily chdir to llmOpt/ so the clone lands
    # in llmOpt/gengin/ rather than ./gengin/ at the repo root.
    import os as _os
    _old_cwd = _os.getcwd()
    try:
        _os.chdir(_llmOpt_dir)
        _main.PROJECT_DIR = "gengin"  # relative to llmOpt/
        _main.git_pull_project()
        _main.PROJECT_DIR = _gengin_dir  # restore absolute path
    finally:
        _os.chdir(_old_cwd)
    _gf.init(base_dir=_gengin_dir)
    return "Project pulled into llmOpt/gengin/ and indexed successfully."


@mcp.tool()
def get_tree(path: str = "") -> str:
    """Return an ASCII directory tree of the project (or a subdirectory if path is given).
    With no arguments, shows the entire project tree."""
    return _main.getTree(path if path else _main.PROJECT_DIR)


@mcp.tool()
def get_todos(path: str = ".") -> str:
    """Grep all TODO comments recursively under path (default: gengin/)."""
    return _main.getTodos(path)


@mcp.tool()
def build_project() -> str:
    """Run make clean && make in the project directory.
    Includes a fast syntax-check pass on changed .c files before the full build
    and an optional auto-review of the diff by a reviewer model.
    Raises RuntimeError on compilation failure."""
    _trimContext()
    return _main.buildProject()


@mcp.tool()
def make_bench() -> str:
    """Run make bench (5 runs, median-aggregated) and compare against baseline.

    Returns a JSON object with:
      - summary: human-readable comparison vs baseline (includes auto-restore
        notification if visual regression triggered rollback)
      - bench_results: median-aggregated scalar metrics
      - raw_stdout_preview: first 2000 chars of the last bench run stdout

    The first call establishes the baseline; subsequent calls compare against it.
    Auto-restores all changes on significant visual regression (MSE >= 50)."""
    _trimContext()

    stdout, bench_raw = _main.makeBench()

    # Extract the full summary from CONTEXT — it includes the auto-restore
    # notification that _benchSummary alone does not capture.
    summary = "No summary available"
    if _main.CONTEXT:
        last = _main.CONTEXT[-1]
        if last.get("tool") == "makeBench":
            summary = last.get("output", summary)

    filtered = {
        k: v for k, v in bench_raw.items()
        if k not in ("frame_images", "frame_hashes")
    }
    return json.dumps({
        "summary": summary,
        "bench_results": filtered,
        "raw_stdout_preview": (stdout or "")[:2000],
    }, indent=2)


@mcp.tool()
def make_flame() -> dict:
    """Run make flame (perf record + flamegraph), parse perf.data, and return
    a dict with total_samples, hot_functions (top 25), and hot_paths (top 20).
    Also refreshes the pinned hotspots for context-aware optimization."""
    _trimContext()
    return _main.makeFlame()


@mcp.tool()
def create_pr(title: str, body: str, branch: str, commit_msg: str = "") -> str:
    """Commit all changes, push to a new branch, and open a GitHub PR via gh CLI.
    Requires GITHUB_TOKEN in environment or .env file.
    Returns the PR URL."""
    _main.createPR(title, body, branch, commit_msg or title)
    # Extract PR URL from CONTEXT (last entry)
    if _main.CONTEXT:
        last = _main.CONTEXT[-1]
        if last.get("tool") == "createPR":
            return last.get("output", "PR created (URL not captured)")
    return "PR created."


# ===================================================================
# Micro-Benchmark Sandbox  (main.py)
# ===================================================================

@mcp.tool()
def create_func_bench(func_name: str, header_code: str, impl_code: str) -> str:
    """Create bench/<func_name>.h and bench/<func_name>.c for standalone
    micro-benchmarking.  impl_code must #include the header and tests/timings.h,
    implement the variants, and contain a main() that times each variant with
    clock_gettime, calls ComputePerformanceMetrics(), and validates correctness."""
    return _main.createFuncBench(func_name, header_code, impl_code)


@mcp.tool()
def run_func_bench(func_name: str) -> str:
    """Build and run the micro-benchmark for func_name via make benchFunc.
    Returns stdout with timing results and validation output."""
    return _main.runFuncBench(func_name)


@mcp.tool()
def run_perf_stat(func_name: str) -> str:
    """Run perf stat on a micro-benchmark binary to collect hardware counters:
    cache-misses, cycles, instructions, branches, branch-misses.
    Call AFTER run_func_bench(). Returns parsed counters with IPC, cache-miss
    rate, branch-miss rate, and an interpretation guide."""
    return _main.runPerfStat(func_name)


@mcp.tool()
def delete_func_bench(func_name: str) -> str:
    """Remove bench/<func_name>.h, bench/<func_name>.c, and the compiled binary."""
    return _main.deleteFuncBench(func_name)


# ===================================================================
# Validation & Bisection  (main.py)
# ===================================================================

@mcp.tool()
def review_changes() -> str:
    """Send the current git diff to a fast reviewer model (DeepSeek V4 Flash)
    that checks for compile errors, null derefs, off-by-one, VLA stack
    explosions, cache false sharing, and logic errors.  Returns PASS/FAIL
    with specific issues.  Call AFTER applying edits but BEFORE buildProject().
    Results are cached per unique diff — calling again is free."""
    return _main.reviewChanges()


@mcp.tool()
def bisect_regression() -> str:
    """When make_bench shows a regression, identify which specific edit caused it.
    Uses linear search (1-4 edits) or binary search (5+ edits), running
    make_bench after each.  Reports the exact culprit edit.
    Requires edit snapshots captured during the editing session."""
    return _main.bisectRegression()


# ===================================================================
# Knowledge Persistence  (main.py)
# ===================================================================

@mcp.tool()
def sync_planner_to_codebase_context() -> str:
    """Ask the model to distill planner notes/tasks into high-level insights
    (architectural findings, confirmed wins, remaining hotspots, techniques
    to try/avoid) and inject them into codebase_context.md.  Clears processed
    planner notes so insights survive across runs."""
    return _main.syncPlannerToCodebaseContext()


@mcp.tool()
def get_codebase_context(section: str = "") -> str:
    """Read the persisted codebase knowledge base (codebase_context.md).
    Pass a section heading substring (e.g. 'Performance-Critical Functions')
    to fetch only that part.  Call with no args to read the whole document."""
    return _main.getCodebaseContext(section if section else None)


# ===================================================================
# Exploration  (getFunc.py)
# ===================================================================

@mcp.tool()
def show_context(func: str, depth: int = 1) -> str:
    """Show the target function, its callees (up to `depth` levels), and all
    referenced struct/type definitions with file:line annotations."""
    result = _gf.showContext(func, depth=depth, returnString=True)
    if result is None:
        return f"Function '{func}' not found in codebase."
    return result


@mcp.tool()
def show_context_with_meta(func: str, depth: int = 1) -> dict:
    """Like show_context but returns a structured dict:
    {text: str, functions: [{name, file, start, end}], structs: [{name, file, start, end}]}"""
    result = _gf.showContextWithMeta(func, depth=depth)
    return result if result is not None else {"text": "", "functions": [], "structs": []}


@mcp.tool()
def get_definition(name: str) -> str:
    """Return the full definition of a function or struct/typedef with file:line info."""
    return _gf.getDefinition(name, returnString=True) or ""


@mcp.tool()
def get_callers(func: str) -> str:
    """List every function that calls the given function."""
    return _gf.getCallers(func, returnString=True) or ""


@mcp.tool()
def get_diff(code_only: bool = False) -> str:
    """Show current git diff vs HEAD.  Set code_only=True to filter to .c/.h/.cl/.go/.py files only."""
    return _gf.getDiff(returnString=True, code_only=code_only) or ""


@mcp.tool()
def grep_source(pattern: str, rel_path: str = "", ignore_case: bool = False) -> str:
    """grep a regex/literal pattern across .c/.h/.cl source files.
    Pass rel_path to limit search to a subdirectory."""
    path_arg = rel_path if rel_path else None
    return _gf.grepSource(pattern, rel_path=path_arg, ignore_case=ignore_case, returnString=True) or ""


@mcp.tool()
def find_symbol(name: str) -> str:
    """Word-boundary grep for a symbol name across all source files.
    Finds declarations, definitions, and call sites without partial matches."""
    return _gf.findSymbol(name, returnString=True) or ""


@mcp.tool()
def list_dir(rel_path: str = ".") -> str:
    """List files and subdirectories under rel_path (relative to project root).
    Directories are suffixed with '/'."""
    return _gf.listDir(rel_path, returnString=True) or ""


@mcp.tool()
def read_lines(rel_path: str, start: int, end: int) -> str:
    """Read lines start..end (1-indexed inclusive) from a file with line-number prefixes."""
    return _gf.readLines(rel_path, start, end, returnString=True) or ""


@mcp.tool()
def run_command(cmd_list: list[str]) -> str:
    """Run a whitelisted analysis command in the project directory.
    Allowed: nm, size, objdump, readelf, addr2line, strings, ldd, file, ar, strip,
    objcopy, perf, gprof, valgrind, make, cmake, ninja, gcc, g++, clang, clang++,
    cc, clang-format, clang-tidy, cppcheck, python, python3, go, perl, patch, tee,
    tr, diff, wc, awk, sed, sort, uniq, head, tail, xxd, hexdump, cat.
    Path traversal in arguments is rejected for safety."""
    return _gf.runCommand(cmd_list, returnString=True) or ""


@mcp.tool()
def read_source_file(rel_path: str) -> str:
    """Read the raw content of a source file (no line numbers).
    Path traversal (..) is rejected for safety."""
    # Defense-in-depth guard — mirrors the guards in listDir, readLines,
    # and grepSource, plus an absolute-path check.  readSourceFile in
    # getFunc.py does not have its own guard, so we add one here without
    # modifying getFunc.py.
    err = _checkPath(rel_path)
    if err:
        return err
    return _gf.readSourceFile(rel_path, returnString=True) or ""


@mcp.tool()
def list_functions() -> str:
    """List all indexed functions sorted by file, with signatures."""
    return _gf.listFunctions(returnString=True) or ""


@mcp.tool()
def show_src(rel_path: str) -> str:
    """Print file with N: line prefixes.  Every line number maps directly to
    replace_lines / insert_lines / delete_lines."""
    err = _checkPath(rel_path)
    if err:
        return err
    return _gf.showSrc(rel_path, returnString=True) or ""


@mcp.tool()
def show_src_pair(rel_path: str) -> str:
    """Print a .c or .h file together with its companion (auto-finds by
    swapping the extension)."""
    err = _checkPath(rel_path)
    if err:
        return err
    return _gf.showSrcPair(rel_path, returnString=True) or ""


# ===================================================================
# Hotspot Annotation  (getFunc.py)
# ===================================================================

@mcp.tool()
def hot_annotate_func(func_name: str, threshold: float = 0.5) -> str:
    """Return the source of func_name annotated with /* HOT X.X% */ markers
    on lines consuming >= threshold% of perf samples.
    Requires perf.data from a previous make_flame() call."""
    return _gf.hotAnnotateFunc(func_name, threshold=threshold) or ""


@mcp.tool()
def hot_annotate_file(rel_path: str, threshold: float = 0.5) -> str:
    """Return an entire source file annotated with /* HOT X.X% */ per-line
    hotness across all functions in it.
    Requires perf.data from a previous make_flame() call."""
    return _gf.hotAnnotateFile(rel_path, threshold=threshold) or ""


# ===================================================================
# Editing  (getFunc.py)
# ===================================================================

@mcp.tool()
def search_replace(rel_path: str, old_text: str, new_text: str,
                   occurrence: int = 0) -> str:
    """Find old_text in rel_path and replace it with new_text.
    Matching strategy: exact -> whitespace-normalized -> fuzzy (with diff hints).
    When occurrence=0 (default), old_text must be unique (exactly 1 match).
    Set occurrence=N to target the N-th match of a repeated pattern (1-indexed).

    After a successful edit, captures a snapshot for bisect_regression()."""
    occ = occurrence if occurrence > 0 else None
    result = _ctxCall(_gf.searchReplace, rel_path, old_text, new_text, occurrence=occ)
    if "replaced" in result.lower():
        _main._captureEditSnapshot(f"search_replace in {rel_path}")
    return result


@mcp.tool()
def preview_change(rel_path: str, old_text: str, new_text: str) -> str:
    """Show a unified diff of what search_replace WOULD change, without
    modifying the file.  Use before applying edits to verify correctness."""
    return _gf.previewChange(rel_path, old_text, new_text)


@mcp.tool()
def search_replace_multi(rel_path: str, replacements: list[dict]) -> str:
    """Apply multiple find-and-replace pairs in one call, top-to-bottom.
    replacements = [{"old": str, "new": str}, ...].
    ALL replacements are validated against the current file BEFORE any are
    written — if any would fail, NONE are applied (transaction safety).

    After a successful edit, captures a snapshot for bisect_regression()."""
    result = _ctxCall(_gf.searchReplaceMulti, rel_path, replacements)
    if "applied" in result.lower():
        _main._captureEditSnapshot(f"search_replace_multi in {rel_path}")
    return result


@mcp.tool()
def apply_change(func_name: str, new_definition: str) -> str:
    """Replace a named function in-place by locating its signature in the
    source file.  The new_definition should be the complete function body
    including the signature and braces.

    After a successful edit, captures a snapshot for bisect_regression()."""
    result = _ctxCall(_gf.applyChange, func_name, new_definition)
    if "replaced" in result.lower():
        _main._captureEditSnapshot(f"apply_change on {func_name}")
    return result


@mcp.tool()
def replace_lines(rel_path: str, start: int, end: int, new_text: str) -> str:
    """Replace lines start..end (1-indexed inclusive) with new_text.
    Pairs with show_src line numbers.

    After a successful edit, captures a snapshot for bisect_regression()."""
    result = _ctxCall(_gf.replaceLines, rel_path, start, end, new_text)
    if "replaced" in result.lower():
        _main._captureEditSnapshot(f"replace_lines {start}-{end} in {rel_path}")
    return result


@mcp.tool()
def insert_lines(rel_path: str, after_line: int, new_text: str) -> str:
    """Insert new_text after after_line (1-indexed).  Use after_line=0 to
    prepend at the top of the file.

    After a successful edit, captures a snapshot for bisect_regression()."""
    result = _ctxCall(_gf.insertLines, rel_path, after_line, new_text)
    if "inserted" in result.lower():
        _main._captureEditSnapshot(f"insert_lines after {after_line} in {rel_path}")
    return result


@mcp.tool()
def delete_lines(rel_path: str, start: int, end: int) -> str:
    """Delete lines start..end (1-indexed inclusive) from rel_path.

    After a successful edit, captures a snapshot for bisect_regression()."""
    result = _ctxCall(_gf.deleteLines, rel_path, start, end)
    if "deleted" in result.lower():
        _main._captureEditSnapshot(f"delete_lines {start}-{end} in {rel_path}")
    return result


@mcp.tool()
def apply_patch(patches: list[dict]) -> str:
    """Batch line-range replacement.  patches = [{file, start, end, text}, ...].
    Applied in reverse line order per file so earlier patches don't shift
    later line indices.

    After a successful edit, captures a snapshot for bisect_regression()."""
    ctx: list = []
    success = _gf.applyPatch(patches, context=ctx)
    result = ctx[-1].get("output", str(success)) if ctx else str(success)
    if success:
        _main._captureEditSnapshot(f"apply_patch ({len(patches)} regions)")
    return result


# ===================================================================
# Git & Restore  (getFunc.py)
# ===================================================================

@mcp.tool()
def restore_all() -> str:
    """git checkout HEAD -- . to discard ALL uncommitted changes.
    Also clears the edit snapshot stack used by bisect_regression()."""
    success = _gf.restoreAll()
    _main._clearEditStack()
    return "All files restored to HEAD." if success else "restoreAll failed."


@mcp.tool()
def restore_file(rel_path: str) -> str:
    """Restore a single file to HEAD (git checkout HEAD -- <rel_path>)."""
    success = _gf.restoreFile(rel_path)
    return f"Restored {rel_path} to HEAD." if success else f"Failed to restore {rel_path}."


@mcp.tool()
def restore_function(func_name: str) -> str:
    """Restore the source file containing the named function to HEAD."""
    success = _gf.restoreFunction(func_name)
    return f"Restored file containing {func_name} to HEAD." if success else f"Failed to restore {func_name}."


@mcp.tool()
def api_help() -> str:
    """Print all available API function signatures with one-line descriptions,
    grouped by category.  Use this to discover tools at runtime."""
    return _gf.apiHelp(returnString=True) or ""


def _symRange(sym: dict) -> dict:
    """Extract the full range from a clangd symbol, handling both formats:
    - Hierarchical: sym.range.start/end
    - Flat (SymbolInformation): sym.location.range.start/end
    Falls back to selectionRange if range is not available."""
    loc = sym.get("location", {})
    rng = sym.get("range") or loc.get("range") or sym.get("selectionRange", {})
    return rng


def _symName(sym: dict) -> str:
    return sym.get("name", "?")


def _symKind(sym: dict) -> int:
    return sym.get("kind", 0)


# ===================================================================
# LSP / clangd Tools  (lsp_client.py)
# ===================================================================

@mcp.tool()
def lsp_references(symbol: str, rel_path: str) -> str:
    """Find ALL semantic references to a symbol using clangd (more accurate
    than regex find_symbol).  Returns file:line for every reference."""
    pos = _symbolPosition(symbol, rel_path)
    if pos is None:
        return f"Symbol '{symbol}' not found in the codebase index."
    f, line, char = pos
    client = _getLspClient()
    refs = client.references(f, line, char)
    if refs is None:
        return "LSP: clangd not available or request failed."
    if not refs:
        return f"No references found for '{symbol}'."
    lines = [f"{len(refs)} reference(s) to '{symbol}':"]
    for r in refs:
        lines.append(f"  {_fmtLocation(r['uri'], r['range'])}")
    return "\n".join(lines)


@mcp.tool()
def lsp_definition(symbol: str, rel_path: str) -> str:
    """Go to the exact AST definition of a symbol.  More precise than
    the regex-based get_definition."""
    pos = _symbolPosition(symbol, rel_path)
    if pos is None:
        return f"Symbol '{symbol}' not found in the codebase index."
    f, line, char = pos
    client = _getLspClient()
    defs = client.definition(f, line, char)
    if defs is None:
        return "LSP: clangd not available or request failed."
    if not defs:
        return f"No definition found for '{symbol}'."
    lines = [f"Definition of '{symbol}':"]
    for d in defs:
        lines.append(f"  {_fmtLocation(d['uri'], d['range'])}")
    return "\n".join(lines)


@mcp.tool()
def lsp_rename(symbol: str, rel_path: str, new_name: str) -> str:
    """Workspace-wide semantic rename.  clangd updates ALL references across
    all files atomically — no text matching, no missed references."""
    pos = _symbolPosition(symbol, rel_path)
    if pos is None:
        return f"Symbol '{symbol}' not found in the codebase index."
    f, line, char = pos
    client = _getLspClient()
    prep = client.prepareRename(f, line, char)
    if prep is None:
        return f"Cannot rename '{symbol}' at {f}:{line+1} — clangd rejected the location."
    result = client.rename(f, line, char, new_name)
    if result is None:
        return f"Rename of '{symbol}' to '{new_name}' failed."
    changes = result.get("changes", {})
    if not changes:
        return f"Rename succeeded but no files changed (already named '{new_name}'?)."
    total = sum(len(edits) for edits in changes.values())
    lines = [f"Renamed '{symbol}' -> '{new_name}' ({total} change(s) across {len(changes)} file(s)):"]
    for uri, edits in sorted(changes.items()):
        lines.append(f"  {uri.replace('file://', '')}: {len(edits)} edit(s)")
    _main._captureEditSnapshot(f"lsp_rename {symbol} -> {new_name}")
    return "\n".join(lines)


@mcp.tool()
def lsp_symbol_range(symbol: str, rel_path: str) -> str:
    """Get EXACT line range for a symbol from the AST.  Returns JSON with
    start_line, end_line (1-indexed) — feed directly into replace_lines()
    for zero-risk editing without text matching."""
    client = _getLspClient()
    syms = client.documentSymbol(rel_path)
    if syms is None:
        return "LSP: clangd not available or request failed."
    for s in syms:
        if _symName(s) == symbol:
            rng = _symRange(s)
            start = rng.get("start", {})
            end = rng.get("end", {})
            import json as _json
            return _json.dumps({
                "symbol": symbol,
                "file": rel_path,
                "start_line": start.get("line", 0) + 1,
                "start_char": start.get("character", 0),
                "end_line": end.get("line", 0) + 1,
                "end_char": end.get("character", 0),
            }, indent=2)
    return f"Symbol '{symbol}' not found in {rel_path}."


@mcp.tool()
def lsp_diagnostics(rel_path: str) -> str:
    """Get compiler warnings/errors for a file via clangd publishDiagnostics.
    Much faster than build_project (~1s vs ~30s).  File must have been opened
    first (call lsp_symbols on it to trigger parsing)."""
    client = _getLspClient()
    diags = client.diagnostics(rel_path)
    if diags is None:
        return "LSP: clangd not available."
    if not diags:
        return f"No diagnostics for {rel_path} — file is clean."
    lines = [f"Diagnostics for {rel_path} ({len(diags)} issue(s)):"]
    for d in sorted(diags, key=lambda x: (x.get('range', {}).get('start', {}).get('line', 0), x.get('severity', 4))):
        sev = _fmtSeverity(d.get("severity", 4))
        msg = d.get("message", "")
        line = d.get("range", {}).get("start", {}).get("line", 0) + 1
        lines.append(f"  {rel_path}:{line}: [{sev}] {msg}")
    return "\n".join(lines)


@mcp.tool()
def lsp_diagnostics_all() -> str:
    """Get diagnostics for all opened files.  Open files first with lsp_symbols
    or lsp_show_context to trigger clangd parsing."""
    client = _getLspClient()
    all_diags = client.diagnosticsAll()
    if all_diags is None:
        return "LSP: clangd not available."
    total = sum(len(v) for v in all_diags.values())
    if total == 0:
        return "No diagnostics — all opened files are clean."
    lines = [f"Diagnostics across {len(all_diags)} file(s) ({total} issue(s)):"]
    for fname, diags in sorted(all_diags.items()):
        fname_short = fname.replace('file://', '')
        for d in sorted(diags, key=lambda x: x.get('range', {}).get('start', {}).get('line', 0)):
            sev = _fmtSeverity(d.get("severity", 4))
            msg = d.get("message", "")
            line = d.get("range", {}).get("start", {}).get("line", 0) + 1
            lines.append(f"  {fname_short}:{line}: [{sev}] {msg}")
    return "\n".join(lines)


@mcp.tool()
def lsp_symbols(rel_path: str) -> str:
    """Get a structured symbol outline for a file from clangd.  Returns every
    function, struct, and macro with exact line ranges.  LSP alternative to
    list_functions — per-file, AST-accurate, with line spans."""
    client = _getLspClient()
    syms = client.documentSymbol(rel_path)
    if syms is None:
        return "LSP: clangd not available or request failed."
    if not syms:
        return f"No symbols found in {rel_path}."

    _KIND = {12: "function", 22: "struct", 26: "macro", 13: "variable",
             6: "method", 5: "class", 11: "interface", 14: "constant",
             9: "constructor", 8: "field", 7: "property"}

    lines = [f"Symbols in {rel_path}:"]
    for s in syms:
        name = _symName(s)
        kind = _KIND.get(_symKind(s), f"kind{_symKind(s)}")
        rng = _symRange(s)
        sl = rng.get("start", {}).get("line", 0) + 1
        el = rng.get("end", {}).get("line", 0) + 1
        lines.append(f"  {name} [{kind}]  lines {sl}-{el}")
    return "\n".join(lines)


@mcp.tool()
def lsp_show_context(symbol: str, rel_path: str) -> str:
    """LSP-powered show_context.  Shows definition, callers (LSP first, regex
    fallback), callees, references, and exact symbol range for editing.
    More accurate than the regex show_context for navigation."""
    lines = []

    # 1. Definition
    lines.append("=== DEFINITION ===")
    lines.append(lsp_definition(symbol, rel_path))

    # 2. Symbol range (for editing -- feed into replace_lines)
    lines.append("\n=== SYMBOL RANGE (for replace_lines) ===")
    lines.append(lsp_symbol_range(symbol, rel_path))

    # 3. Callers -- try LSP call hierarchy first, fall back to regex
    lines.append("\n=== CALLERS (who calls this) ===")
    ch_result = lsp_call_hierarchy(symbol, rel_path, "incoming")
    if "not available" in ch_result.lower():
        fallback = _gf.getCallers(symbol, returnString=True)
        lines.append(fallback if fallback else "(no callers found)")
    else:
        lines.append(ch_result)

    # 4. Callees -- LSP only (no regex equivalent)
    lines.append("\n=== CALLEES (what this calls) ===")
    ch_out = lsp_call_hierarchy(symbol, rel_path, "outgoing")
    if "not available" in ch_out.lower():
        lines.append("(call hierarchy not supported by this clangd version)")
    else:
        lines.append(ch_out)

    # 5. All references
    lines.append("\n=== ALL REFERENCES ===")
    lines.append(lsp_references(symbol, rel_path))

    return "\n".join(lines)


@mcp.tool()
def lsp_get_callers(symbol: str, rel_path: str) -> str:
    """Simple wrapper: list all functions that call this symbol.
    LSP alternative to get_callers — semantic, not regex-based."""
    return lsp_call_hierarchy(symbol, rel_path, "incoming")


@mcp.tool()
def lsp_get_callees(symbol: str, rel_path: str) -> str:
    """Simple wrapper: list all functions called by this symbol.
    Useful for understanding what a function depends on before editing."""
    return lsp_call_hierarchy(symbol, rel_path, "outgoing")


@mcp.tool()
def lsp_file_symbols(rel_path: str) -> str:
    """Alias for lsp_symbols — get the complete symbol outline of a file.
    Every function, struct, and macro with AST-exact line ranges."""
    return lsp_symbols(rel_path)


@mcp.tool()
def lsp_hover(symbol: str, rel_path: str) -> str:
    """Get type information and documentation for a symbol via clangd hover.
    Shows the deduced type, parameter types, and any doc comments."""
    pos = _symbolPosition(symbol, rel_path)
    if pos is None:
        return f"Symbol '{symbol}' not found in the codebase index."
    f, line, char = pos
    client = _getLspClient()
    # clangd hover returns {contents: {kind, value}} or {contents: [...]}
    result = client._request("textDocument/hover", {
        "textDocument": {"uri": f"file://{os.path.join(_gengin_dir, f)}"},
        "position": {"line": line, "character": char},
    })
    if result is None:
        return f"No hover info available for '{symbol}'."
    contents = result.get("contents", {})
    if isinstance(contents, dict):
        return contents.get("value", str(contents))
    if isinstance(contents, list):
        return "\n".join(
            c.get("value", str(c)) if isinstance(c, dict) else str(c)
            for c in contents
        )
    return str(contents) if contents else f"No hover info for '{symbol}'."


@mcp.tool()
def lsp_call_hierarchy(symbol: str, rel_path: str, direction: str = "incoming") -> str:
    """Show the call hierarchy for a function.  direction='incoming' = who calls
    this; direction='outgoing' = what this calls.  Understand blast radius
    before editing hot-path code."""
    pos = _symbolPosition(symbol, rel_path)
    if pos is None:
        return f"Symbol '{symbol}' not found in the codebase index."
    f, line, char = pos
    if direction not in ("incoming", "outgoing"):
        return f"direction must be 'incoming' or 'outgoing', got {direction!r}."
    client = _getLspClient()
    calls = client.callHierarchy(f, line, char, direction)
    if calls is None:
        return f"Call hierarchy not available for '{symbol}' — may not be a function or clangd does not support it for this symbol."
    if not calls:
        label = "callers" if direction == "incoming" else "callees"
        return f"No {label} found for '{symbol}'."
    label = f"Callers of '{symbol}'" if direction == "incoming" else f"Functions called by '{symbol}'"
    lines = [f"{label} ({len(calls)}):"]
    for c in calls:
        if direction == "incoming":
            caller = c.get("from", {})
            name = caller.get("name", "?")
            uri = caller.get("uri", "")
            rng = caller.get("range", {})
            lines.append(f"  {name}  ({_fmtLocation(uri, rng)})")
        else:
            callee = c.get("to", {})
            name = callee.get("name", "?")
            uri = callee.get("uri", "")
            rng = callee.get("range", {})
            lines.append(f"  {name}  ({_fmtLocation(uri, rng)})")
    return "\n".join(lines)


@mcp.tool()
def lsp_implementations(symbol: str, rel_path: str) -> str:
    """Find all implementations of a symbol (function defs for a header
    declaration, macro expansions, etc.)."""
    pos = _symbolPosition(symbol, rel_path)
    if pos is None:
        return f"Symbol '{symbol}' not found in the codebase index."
    f, line, char = pos
    client = _getLspClient()
    impls = client.implementation(f, line, char)
    if impls is None:
        return "LSP: clangd not available or request failed."
    if not impls:
        return f"No implementations found for '{symbol}'."
    lines = [f"{len(impls)} implementation(s) of '{symbol}':"]
    for imp in impls:
        lines.append(f"  {_fmtLocation(imp['uri'], imp['range'])}")
    return "\n".join(lines)


# ===================================================================
# Model Selection
# ===================================================================

@mcp.tool()
def set_model_pro() -> str:
    """Switch internal LLM calls (code review, research, planner sync) to
    deepseek/deepseek-v4-pro via DeepSeek provider.  Higher quality, slower,
    better for complex reasoning.  This is the DEFAULT model."""
    return _mc.setModel(_mc.PRO)


@mcp.tool()
def set_model_flash() -> str:
    """Switch internal LLM calls to deepseek/deepseek-v4-flash via DeepSeek
    provider.  Faster and cheaper, good for simple reviews and summarization.
    Use this to save tokens during iterative optimization loops."""
    return _mc.setModel(_mc.FLASH)


@mcp.tool()
def get_model_config() -> str:
    """Return the current model configuration as JSON: active model, provider,
    available models, and which model is used for each internal LLM task
    (review, research, sync)."""
    import json as _json
    return _json.dumps(_mc.getConfig(), indent=2)


# ===================================================================
# Entry point
# ===================================================================

if __name__ == "__main__":
    print("gengin-optimizer MCP server starting (stdio transport)...", file=sys.stderr)
    print(file=sys.stderr)
    print("OpenCode config (place in opencode.json at project root):", file=sys.stderr)
    print("""
{
  "mcp": {
    "gengin-optimizer": {
      "type": "local",
      "command": ["python", "llmOpt/mcp_server.py"],
      "enabled": true,
      "timeout": 60000
    }
  }
}
""", file=sys.stderr)
    print("--", file=sys.stderr)
    print("Set your OpenRouter API key:", file=sys.stderr)
    print("  export OPENROUTER_API_KEY=sk-or-v1-...", file=sys.stderr)
    print("Then run:", file=sys.stderr)
    print("  opencode run --model openrouter/deepseek/deepseek-v4-flash \"your prompt\"", file=sys.stderr)
    print("--", file=sys.stderr)
    print(f"Indexed {len(_gf._functions)} functions in {len(_gf._sources)} source files.", file=sys.stderr)
    print("Ready.", file=sys.stderr)
    mcp.run()
