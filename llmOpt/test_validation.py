#!/usr/bin/env python3
"""
Comprehensive validation suite for llmOpt optimization loop improvements.
Tests: model_config, research_agent, skeptical review, createPR gate, edge cases.
"""
import sys, os, json, re

# Ensure llmOpt is on path
_llmOpt_dir = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _llmOpt_dir)

passed = 0
failed = 0
errors = []

def check(desc, condition):
    global passed, failed
    if condition:
        passed += 1
        print(f"  PASS: {desc}")
    else:
        failed += 1
        msg = f"  FAIL: {desc}"
        print(msg)
        errors.append(msg)

def section(title):
    print(f"\n{'='*60}")
    print(f"  {title}")
    print(f"{'='*60}")

# ============================================================================
section("1. model_config.py -- Three-Role Architecture")

import model_config as mc

cfg = mc.getConfig()
check("getConfig returns 'roles' key", "roles" in cfg)
check("Three roles exist", set(cfg["roles"].keys()) == {"main", "review", "research"})
check("Main model defaults to PRO", cfg["roles"]["main"]["model"] == mc.PRO)
check("Review model defaults to FLASH", cfg["roles"]["review"]["model"] == mc.FLASH)
check("Research model defaults to FLASH", cfg["roles"]["research"]["model"] == mc.FLASH)
check("All providers default to deepseek",
      all(cfg["roles"][r]["provider"] == "deepseek" for r in cfg["roles"]))

check("getModel() returns main model", mc.getModel() == mc.PRO)
check("getReviewModel() returns FLASH", mc.getReviewModel() == mc.FLASH)
check("getResearchModel() returns FLASH", mc.getResearchModel() == mc.FLASH)
check("getProvider() returns deepseek", mc.getProvider() == "deepseek")
check("getReviewProvider() returns deepseek", mc.getReviewProvider() == "deepseek")
check("getResearchProvider() returns deepseek", mc.getResearchProvider() == "deepseek")

# Setters
old_review = mc.getReviewModel()
result = mc.setReviewModel("test/model-x")
check("setReviewModel returns confirmation", "test/model-x" in result)
check("setReviewModel changes the model", mc.getReviewModel() == "test/model-x")
mc.setReviewModel(old_review)
check("setReviewModel restore works", mc.getReviewModel() == old_review)

old_research = mc.getResearchModel()
mc.setResearchModel("test/model-y")
check("setResearchModel changes the model", mc.getResearchModel() == "test/model-y")
mc.setResearchModel(old_research)

old_rprov = mc.getReviewProvider()
mc.setReviewProvider("test-provider")
check("setReviewProvider works", mc.getReviewProvider() == "test-provider")
mc.setReviewProvider(old_rprov)

# setModel backward compat
result = mc.setModel(mc.FLASH)
check("setModel (main) returns confirmation", "deepseek-v4-flash" in result)
mc.setModel(mc.PRO)

# Bridge to main.py
import main as _main
check("REVIEWER_MODEL bridged", _main.REVIEWER_MODEL == mc.getReviewModel())
check("RESEARCH_MODEL bridged", _main.RESEARCH_MODEL == mc.getResearchModel())
check("SKEPTICAL_REVIEW_MODEL exists in main.py", hasattr(_main, 'SKEPTICAL_REVIEW_MODEL'))

# ============================================================================
section("2. research_agent.py -- Tool Map & Sandboxing")

import research_agent as ra

tool_map = ra._makeToolMap()
check("Tool map is a dict", isinstance(tool_map, dict))
check("Tool map has 22 tools", len(tool_map) == 22)
check("Each tool entry is (callable, description)",
      all(isinstance(v, tuple) and len(v) == 2 and callable(v[0]) and isinstance(v[1], str)
          for v in tool_map.values()))

# Verify NO editing tools leaked in
EDITING_TOOLS = {"search_replace", "preview_change", "search_replace_multi",
                 "apply_change", "replace_lines", "insert_lines", "delete_lines",
                 "apply_patch", "lsp_rename"}
STATE_CHANGING = {"build_project", "make_bench", "create_pr", "restore_all",
                  "restore_file", "restore_function", "create_func_bench",
                  "delete_func_bench", "review_changes", "skeptical_review",
                  "bisect_regression", "sync_planner_to_codebase_context",
                  "git_pull_project"}
for t in EDITING_TOOLS:
    check(f"Editing tool '{t}' NOT in research agent", t not in tool_map)
for t in STATE_CHANGING:
    check(f"State-changing tool '{t}' NOT in research agent", t not in tool_map)

# Verify expected read-only tools ARE present
EXPECTED = {"show_context", "get_definition", "get_callers", "grep_source",
            "find_symbol", "list_dir", "read_lines", "read_source_file",
            "list_functions", "show_src", "show_src_pair", "get_tree", "get_todos",
            "make_flame", "hot_annotate_func", "hot_annotate_file",
            "run_func_bench", "run_perf_stat", "get_codebase_context",
            "lsp_diagnostics", "lsp_symbols", "lsp_hover"}
for t in EXPECTED:
    check(f"Read-only tool '{t}' IS in research agent", t in tool_map)

# Tool descriptions non-empty
for name, (_, desc) in tool_map.items():
    check(f"Tool '{name}' has non-empty description", len(desc) > 10)

# System prompt
prompt = ra.RESEARCH_SYSTEM_PROMPT
check("System prompt mentions 'READ-ONLY'", "READ-ONLY" in prompt)
check("System prompt mentions 'FINAL REPORT FORMAT'", "FINAL REPORT FORMAT" in prompt)
check("System prompt > 500 chars", len(prompt) > 500)

tool_descs = ra._buildToolDescriptions(tool_map)
check("Tool descriptions > 500 chars", len(tool_descs) > 500)
check("Tool descriptions mention 'Available Tools'", "Available Tools" in tool_descs)
check("Tool descriptions mention 'How to Call Tools'", "How to Call Tools" in tool_descs)

check("MAX_ITERATIONS is 15", ra.MAX_ITERATIONS == 15)

# ============================================================================
section("3. main.py -- Skeptical Review Infrastructure")

sk = _main.SKEPTICAL_REVIEW_PROMPT
check("SKEPTICAL_REVIEW_PROMPT is string", isinstance(sk, str))
check("SKEPTICAL_REVIEW_PROMPT > 300 chars", len(sk) > 300)
check("Says 'ASSUME THE CHANGE IS WRONG'", "ASSUME THE CHANGE IS WRONG" in sk)
check("Mentions compile errors", "FAIL TO COMPILE" in sk)
check("Mentions performance regressions", "HURT PERFORMANCE" in sk)
check("Mentions thread safety", "THREAD SAFETY" in sk)
check("Requests JSON output", '"verdict"' in sk)

check("_skepticalReview callable", callable(_main._skepticalReview))
check("skepticalReview callable", callable(_main.skepticalReview))
check("Separate cache var exists", hasattr(_main, '_last_skeptical_diff_hash'))
check("createPR still callable", callable(_main.createPR))

# Existing review infra untouched
check("REVIEWER_MODEL still exists", hasattr(_main, 'REVIEWER_MODEL'))
check("reviewChanges still callable", callable(_main.reviewChanges))
check("_reviewDiff still callable", callable(_main._reviewDiff))
check("_autoReviewBeforeBuild still callable", callable(_main._autoReviewBeforeBuild))

# ============================================================================
section("4. Edge Cases & Error Handling")

# setModel backward compat with unknown model
result = mc.setModel("unknown-model-xyz")
check("setModel unknown model returns string", isinstance(result, str) and len(result) > 0)

# setReviewModel accepts custom strings
old_r = mc.getReviewModel()
mc.setReviewModel("any-custom-model")
check("setReviewModel accepts custom strings", mc.getReviewModel() == "any-custom-model")
mc.setReviewModel(old_r)

# _resolveProjectDir
proj_dir = ra._resolveProjectDir()
check("_resolveProjectDir returns string", isinstance(proj_dir, str))

# Invalid JSON in tool block
_CMD_RE = re.compile(r'```json\s*(\{.*?\})\s*```', re.DOTALL)
bad_json = "```json\n{invalid json here\n```"
cmds = _CMD_RE.findall(bad_json)
if cmds:
    try:
        json.loads(cmds[0])
        check("Invalid JSON parsed without error (unexpected)", False)
    except json.JSONDecodeError:
        check("Invalid JSON raises JSONDecodeError", True)
else:
    check("Invalid JSON not matched by regex (safe)", True)

# Conversation truncation logic
conv = [{"role": "system", "content": "sys"}, {"role": "user", "content": "q"}]
for i in range(30):
    conv.append({"role": "assistant", "content": f"a{i}"})
    conv.append({"role": "tool_results", "content": f"r{i}"})
if len(conv) > 22:
    conv = [conv[0], conv[1]] + conv[-20:]
check("Truncation keeps 22 msgs", len(conv) == 22)
check("First msg preserved", conv[0]["content"] == "sys")
check("Second msg preserved", conv[1]["content"] == "q")

# Env var override
old_main_model = mc._models["main"]["model"]
try:
    os.environ["GENGIN_MAIN_MODEL"] = "env-test-model"
    mc._envOverride("main")
    check("Env var overrides main model", mc._models["main"]["model"] == "env-test-model")
finally:
    del os.environ["GENGIN_MAIN_MODEL"]
    mc._models["main"]["model"] = old_main_model

# Env var override for review model
old_review_model = mc._models["review"]["model"]
try:
    os.environ["GENGIN_REVIEW_MODEL"] = "env-review-test"
    mc._envOverride("review")
    check("Env var overrides review model", mc._models["review"]["model"] == "env-review-test")
finally:
    del os.environ["GENGIN_REVIEW_MODEL"]
    mc._models["review"]["model"] = old_review_model

# ============================================================================
section("5. Integration -- MCP Server & createPR Wiring")

mcp_path = os.path.join(_llmOpt_dir, "mcp_server.py")
with open(mcp_path) as f:
    mcp_src = f.read()
check("mcp_server has def skeptical_review", "def skeptical_review()" in mcp_src)
check("mcp_server has def research_agent", "def research_agent(research_prompt" in mcp_src)
check("mcp_server imports research_agent", "import research_agent" in mcp_src)

main_path = os.path.join(_llmOpt_dir, "main.py")
with open(main_path) as f:
    main_src = f.read()
check("createPR calls skepticalReview()", "skepticalReview()" in main_src)
check("createPR checks for CRITICAL", "CRITICAL" in main_src and "has_critical" in main_src)
check("createPR has PR ABORTED message", "PR ABORTED" in main_src)

# ============================================================================
section("6. optimize.md -- Prompt Integration")

opt_path = os.path.join(_llmOpt_dir, "prompts", "optimize.md")
with open(opt_path) as f:
    opt_src = f.read()
check("optimize.md mentions research_agent", "research_agent" in opt_src)
check("optimize.md mentions skeptical_review()", "skeptical_review()" in opt_src)
check("optimize.md notes create_pr auto-runs review",
      "auto-runs" in opt_src.lower() and "skeptical_review" in opt_src.lower())

# ============================================================================
print(f"\n{'='*60}")
print(f"  RESULTS: {passed} passed, {failed} failed")
print(f"{'='*60}")
if failed > 0:
    print("\nFAILURES:")
    for e in errors:
        print(f"  {e}")
    sys.exit(1)
else:
    print("\n  All tests passed.")
