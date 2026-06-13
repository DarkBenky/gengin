"""
refine.py -- recursive iterative refinement engine.

Orchestrates nested optimization attempts with:
  1. Attempt history tracking — what was tried, what failed, what was learned
  2. Hierarchical decomposition — breaking complex functions into sub-components
  3. Progressive deepening — from coarse algorithmic changes to fine micro-optimizations
  4. Convergence detection — knowing when further optimization yields diminishing returns
  5. Failure analysis — structured diagnosis of why an approach regressed

The refinement state is persisted in planner_state.json alongside tasks/notes,
so it survives context compression and script restarts.
"""

import json
import os
import time

_STATE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "planner_state.json")

# Optimization strategy taxonomy — ordered from highest to lowest expected impact
STRATEGIES = [
    "algorithmic",       # change the algorithm (O(n^2) -> O(n log n), etc.)
    "memory_layout",     # SoA/AoS, cache line alignment, padding
    "simd",              # SSE/AVX2/AVX-512 vectorization
    "threading",         # parallelism, work distribution, lock reduction
    "branchless",        # remove branches, predication, lookup tables
    "precompute",        # precompute values, LUTs, memoization
    "loop_transform",    # unroll, fuse, interchange, tiling
    "data_reuse",        # temporal/spatial locality improvements
    "instruction_level", # instruction selection, latency hiding, scheduling
]

# Strategies that should be tried first (high impact, low risk)
HIGH_IMPACT_STRATEGIES = {"algorithmic", "memory_layout", "threading"}
# Strategies likely to be eaten by the compiler at -O3 -ffast-math
COMPILER_RESISTANT_STRATEGIES = {"branchless", "instruction_level", "loop_transform"}


def _loadState():
    if not os.path.exists(_STATE_FILE):
        return {"tasks": [], "notes": [], "refinement_log": [], "refinement_state": {}}
    with open(_STATE_FILE) as fh:
        state = json.load(fh)
    state.setdefault("refinement_state", {})
    return state


def _saveState(state):
    with open(_STATE_FILE, "w") as fh:
        json.dump(state, fh, indent=2)


def _funcKey(funcName):
    """Normalize function name to a state key."""
    return funcName.strip()


def initRefinement(funcName, filePath="", hotspotData=None):
    """Initialize refinement tracking for a function. Call once per target."""
    state = _loadState()
    key = _funcKey(funcName)
    if key not in state["refinement_state"]:
        state["refinement_state"][key] = {
            "func_name": funcName,
            "file_path": filePath,
            "depth": 0,
            "attempts": [],
            "sub_components": [],
            "strategies_tried": [],
            "strategies_avoided": [],
            "converged": False,
            "converge_reason": "",
            "best_improvement": None,  # best measured speedup (negative = regression)
            "baseline_time": None,
            "hotspot_data": hotspotData,
            "created_at": time.time(),
        }
        _saveState(state)
    return key


def getRefinementState(funcName):
    """Return the refinement state dict for a function, or None."""
    state = _loadState()
    return state["refinement_state"].get(_funcKey(funcName))


def getAllRefinementStates():
    """Return all active refinement states keyed by function name."""
    state = _loadState()
    return dict(state.get("refinement_state", {}))


def recordRefinementAttempt(funcName, strategy, approach, success, resultSummary="",
                            benchData=None, lessonsLearned="",
                            predictedImprovement=None):
    """
    Record an optimization attempt against a function.
    Called after each micro-benchmark or makeBench validation.
    
    predictedImprovement: optional string like "+15% micro-bench, +5% makeBench"
    The refinement engine will later compare this against actual results.
    """
    state = _loadState()
    key = _funcKey(funcName)
    rs = state["refinement_state"].get(key)
    if not rs:
        return False

    attempt = {
        "timestamp": time.time(),
        "strategy": strategy,
        "approach": approach,
        "success": success,
        "result_summary": resultSummary,
        "bench_data": benchData,
        "lessons_learned": lessonsLearned,
        "predicted_improvement": predictedImprovement,
    }
    rs["attempts"].append(attempt)

    if strategy not in rs["strategies_tried"]:
        rs["strategies_tried"].append(strategy)

    if success and benchData:
        improvement = benchData.get("improvement_pct", 0)
        if rs["best_improvement"] is None or improvement > rs["best_improvement"]:
            rs["best_improvement"] = improvement

    _saveState(state)
    return True


def avoidStrategy(funcName, strategy, reason):
    """Mark a strategy as known-bad for this function with a reason."""
    state = _loadState()
    key = _funcKey(funcName)
    rs = state["refinement_state"].get(key)
    if not rs:
        return False
    entry = {"strategy": strategy, "reason": reason, "timestamp": time.time()}
    rs["strategies_avoided"].append(entry)
    _saveState(state)
    return True


def getUntriedStrategies(funcName):
    """Return strategies not yet attempted or avoided for a function."""
    state = _loadState()
    rs = state["refinement_state"].get(_funcKey(funcName))
    if not rs:
        return list(STRATEGIES)
    tried = set(rs.get("strategies_tried", []))
    avoided = {a["strategy"] for a in rs.get("strategies_avoided", [])}
    excluded = tried | avoided
    # Order: high-impact first, then others, compiler-resistant last
    untried = [s for s in STRATEGIES if s not in excluded]
    return untried


def getRecentFailures(funcName, limit=5):
    """Return recent failed attempts for a function, with lessons learned."""
    state = _loadState()
    rs = state["refinement_state"].get(_funcKey(funcName))
    if not rs:
        return []
    failures = [a for a in rs["attempts"] if not a["success"]]
    failures.sort(key=lambda a: a["timestamp"], reverse=True)
    return failures[:limit]


def getAttemptSummary(funcName):
    """Return a compact summary of all attempts for a function."""
    state = _loadState()
    rs = state["refinement_state"].get(_funcKey(funcName))
    if not rs:
        return "(no refinement data)"

    lines = [f"Refinement history for {funcName}:"]
    successes = sum(1 for a in rs["attempts"] if a["success"])
    failures = sum(1 for a in rs["attempts"] if not a["success"])
    lines.append(f"  {successes} succeeded, {failures} failed "
                 f"(best improvement: {rs.get('best_improvement', 'N/A')})")

    if rs["strategies_tried"]:
        lines.append(f"  Strategies tried: {', '.join(rs['strategies_tried'])}")
    if rs["strategies_avoided"]:
        avoided = [f"{a['strategy']} ({a['reason'][:60]})" for a in rs["strategies_avoided"]]
        lines.append(f"  Strategies avoided: {', '.join(avoided)}")

    for i, a in enumerate(rs["attempts"][-5:]):
        status = "OK" if a["success"] else "FAIL"
        lines.append(f"  [{status}] {a['strategy']}: {a['approach'][:100]}")
        if a.get("lessons_learned"):
            lines.append(f"         learned: {a['lessons_learned'][:120]}")

    if rs.get("converged"):
        lines.append(f"  CONVERGED: {rs.get('converge_reason', '')}")

    return "\n".join(lines)


def convergeFunction(funcName, reason):
    """Mark a function as converged — no more optimization attempts."""
    state = _loadState()
    key = _funcKey(funcName)
    rs = state["refinement_state"].get(key)
    if not rs:
        return False
    rs["converged"] = True
    rs["converge_reason"] = reason
    _saveState(state)
    return True


def shouldConverge(funcName):
    """
    Heuristic to decide if we should stop optimizing a function.
    Returns (should_converge: bool, reason: str).
    """
    state = _loadState()
    rs = state["refinement_state"].get(_funcKey(funcName))
    if not rs:
        return False, ""

    attempts = rs["attempts"]
    if not attempts:
        return False, ""

    # Rule 1: Already marked converged
    if rs.get("converged"):
        return True, rs.get("converge_reason", "previously converged")

    # Rule 2: All strategies exhausted
    untried = getUntriedStrategies(funcName)
    if not untried:
        return True, "all optimization strategies exhausted"

    # Rule 3: Consecutive failures with no progress
    recent = [a for a in attempts[-5:] if not a["success"]]
    if len(recent) >= 5:
        return True, f"{len(recent)} consecutive failed attempts"

    # Rule 4: Best improvement is negative and we've tried high-impact strategies
    if (rs.get("best_improvement") is not None and
            rs["best_improvement"] <= 0 and
            all(s in rs.get("strategies_tried", []) for s in HIGH_IMPACT_STRATEGIES
                if s not in {a["strategy"] for a in rs.get("strategies_avoided", [])})):
        return True, "no improvement from high-impact strategies"

    # Rule 5: Too many total attempts
    if len(attempts) >= 15:
        return True, f"{len(attempts)} total attempts with no decisive win"

    return False, ""


def addSubComponent(funcName, subName, description):
    """Register a sub-component of a function for recursive refinement."""
    state = _loadState()
    key = _funcKey(funcName)
    rs = state["refinement_state"].get(key)
    if not rs:
        return False
    sub = {
        "name": subName,
        "description": description,
        "refined": False,
        "added_at": time.time(),
    }
    rs["sub_components"].append(sub)
    _saveState(state)
    return True


def markSubComponentRefined(funcName, subName):
    """Mark a sub-component as having been refined."""
    state = _loadState()
    rs = state["refinement_state"].get(_funcKey(funcName))
    if not rs:
        return False
    for sub in rs.get("sub_components", []):
        if sub["name"] == subName:
            sub["refined"] = True
            _saveState(state)
            return True
    return False


def getRefinementPromptInjection(funcName):
    """
    Build a prompt injection that injects refinement state into the model's context.
    This guides the model toward trying untried strategies and learning from failures.
    """
    state = _loadState()
    rs = state["refinement_state"].get(_funcKey(funcName))
    if not rs:
        return ""

    parts = []
    parts.append(f"== REFINEMENT STATE: {funcName} ==")

    # What's been tried
    if rs["attempts"]:
        parts.append(f"Attempts so far: {len(rs['attempts'])} "
                     f"({sum(1 for a in rs['attempts'] if a['success'])} succeeded, "
                     f"{sum(1 for a in rs['attempts'] if not a['success'])} failed)")

    # Recent failures with lessons
    failures = getRecentFailures(funcName, limit=3)
    if failures:
        parts.append("Recent failures (DO NOT retry these approaches):")
        for f in failures:
            parts.append(f"  [{f['strategy']}] {f['approach'][:150]}")
            if f.get("lessons_learned"):
                parts.append(f"    Why it failed: {f['lessons_learned'][:200]}")

    # Untried strategies
    untried = getUntriedStrategies(funcName)
    if untried:
        parts.append(f"Untried strategies (try these next): {', '.join(untried[:5])}")
    else:
        parts.append("All strategies exhausted. Consider converging or decomposing.")

    # Strategies to avoid
    if rs["strategies_avoided"]:
        parts.append("Known-bad strategies for this function:")
        for a in rs["strategies_avoided"][-3:]:
            parts.append(f"  AVOID {a['strategy']}: {a['reason'][:150]}")

    # Sub-components
    if rs["sub_components"]:
        unrefined = [s["name"] for s in rs["sub_components"] if not s["refined"]]
        if unrefined:
            parts.append(f"Sub-components still needing refinement: {', '.join(unrefined)}")

    # Convergence check
    should_conv, reason = shouldConverge(funcName)
    if should_conv:
        parts.append(f"CONVERGENCE RECOMMENDED: {reason}")
        parts.append("Call convergeFunction() and move to the next hotspot.")

    return "\n".join(parts)


def getGlobalRefinementSummary():
    """
    Return a compact summary of all active refinement states for the session header.
    """
    states = getAllRefinementStates()
    if not states:
        return ""

    active = {k: v for k, v in states.items() if not v.get("converged")}
    converged = {k: v for k, v in states.items() if v.get("converged")}

    lines = []
    if active:
        lines.append("Active refinement targets:")
        for name, rs in sorted(active.items()):
            attempts = len(rs["attempts"])
            best = rs.get("best_improvement")
            bestStr = f"{best:+.1f}%" if best is not None else "N/A"
            lines.append(f"  {name}: {attempts} attempts, best={bestStr}, "
                         f"untried={len(getUntriedStrategies(name))}")

    if converged:
        lines.append("Converged (will not retry):")
        for name, rs in sorted(converged.items()):
            lines.append(f"  {name}: {rs.get('converge_reason', '')[:100]}")

    return "\n".join(lines) if lines else ""


def buildRefinementPrompt(targetFunc, existingSystemPrompt, flameData=None):
    """
    Build a targeted refinement prompt for a specific function.
    This replaces the generic system prompt when the model is stuck in a
    refinement loop for a particular function.
    """
    state = _loadState()
    rs = state["refinement_state"].get(_funcKey(targetFunc))
    if not rs:
        return existingSystemPrompt

    failures = getRecentFailures(targetFunc, limit=3)
    untried = getUntriedStrategies(targetFunc)

    prompt = f"""\
You are an expert C performance engineer focused on ONE function: {targetFunc}.

Your task is to optimize this function using recursive iterative refinement.
You have already tried several approaches — learn from what failed.

== REFINEMENT STATE ==
Attempts: {len(rs['attempts'])} total
Strategies tried: {', '.join(rs.get('strategies_tried', []) or ['none'])}
Strategies still available: {', '.join(untried[:5]) if untried else 'NONE — consider converging'}

"""

    if failures:
        prompt += "== PREVIOUS FAILURES (do NOT repeat these) ==\n"
        for f in failures:
            prompt += f"- [{f['strategy']}] {f['approach'][:200]}\n"
            if f.get("lessons_learned"):
                prompt += f"  Lesson: {f['lessons_learned'][:200]}\n"

    if rs.get("strategies_avoided"):
        prompt += "\n== KNOWN-BAD APPROACHES ==\n"
        for a in rs["strategies_avoided"][-3:]:
            prompt += f"- {a['strategy']}: {a['reason'][:200]}\n"

    prompt += f"""
== YOUR TASK ==
1. If untried strategies remain: pick the highest-impact untried strategy.
2. Read the function code with showContext().
3. Create a micro-benchmark with createFuncBench() — original + your variant.
4. Run it with runFuncBench(). 
5. If the variant is faster AND correct, apply to main code with searchReplace().
6. If it regresses or fails, call recordRefinementAttempt() to log the failure
   WITH a specific lesson about WHY it failed.
7. If all strategies are exhausted, call convergeFunction() and move on.

{failure_injection}

{existingSystemPrompt}
"""
    return prompt


def getFailureAnalysisPrompt(funcName, failedApproach, benchResult):
    """
    Generate a prompt that asks the model to analyze WHY an optimization failed.
    Used after a micro-bench win but real-world regression.
    """
    return f"""\
You optimized {funcName} with this approach:
  {failedApproach}

The micro-benchmark showed a speedup, but the full makeBench() run showed a
regression. This is a known pattern in this codebase: single-threaded micro-benchmarks
are unreliable for the multi-threaded renderer due to cache pressure, TLB contention,
and memory bandwidth limits.

Bench result: {benchResult}

Analyze WHY this regression happened. Consider:
- Did your change increase the working set size (more memory pressure)?
- Did it add indirection (more cache misses)?
- Did it change alignment or padding (TLB effects)?
- Did it add instructions in a hot loop that the compiler couldn't optimize away?
- Is the micro-benchmark representative of the real workload?

Reply with a concise analysis (2-4 sentences) and a recommendation for what to
try instead (or whether to converge on this function).
"""


def getPredictionAccuracy(funcName):
    """Analyze how well the model's predictions matched actual outcomes."""
    state = _loadState()
    rs = state["refinement_state"].get(_funcKey(funcName))
    if not rs:
        return None

    attempts = rs.get("attempts", [])
    predictions = [a for a in attempts if a.get("predicted_improvement")]
    if not predictions:
        return None

    overestimates = 0
    underestimates = 0
    for a in predictions:
        pred = a.get("predicted_improvement", "")
        if "regress" in str(a.get("result_summary", "")).lower() or not a.get("success"):
            overestimates += 1  # predicted improvement but got regression
        elif a.get("success"):
            underestimates += 1  # prediction was conservative

    total = len(predictions)
    overconfidence = overestimates / total if total > 0 else 0

    if overconfidence > 0.5:
        return (f"Calibration: OVERCONFIDENT ({overestimates}/{total} predictions "
                f"were optimistic — expected gains but got regressions). "
                f"Be MORE conservative in your predictions and pre-mortems.")
    elif overconfidence == 0:
        return (f"Calibration: WELL-CALIBRATED ({total} predictions, "
                f"none overconfident). Your pre-mortems are working.")
    return None


def getCalibrationGuidance():
    """Return calibration guidance across all functions."""
    states = getAllRefinementStates()
    all_overestimates = 0
    all_predictions = 0
    guidance = []

    for name, rs in states.items():
        result = getPredictionAccuracy(name)
        if result:
            guidance.append(f"  {name}: {result}")

    if guidance:
        return "== PREDICTION CALIBRATION ==\n" + "\n".join(guidance[-5:])

    return ""


# Tool wrappers for executor.py registration
def toolRecordRefinementAttempt(func_name, strategy, approach, success,
                                result_summary="", lessons_learned="", context=None):
    """Tool: recordRefinementAttempt(func_name, strategy, approach, success, ...)"""
    ok = recordRefinementAttempt(func_name, strategy, approach, success,
                                  result_summary, lessons_learned=lessons_learned)
    msg = f"Recorded {'successful' if success else 'failed'} attempt on {func_name}: {approach[:80]}"
    if context is not None:
        context.append({"type": "tool_use", "tool": "recordRefinementAttempt",
                       "input": {"func_name": func_name, "strategy": strategy},
                       "output": msg})
    return msg


def toolConvergeFunction(func_name, reason="", context=None):
    """Tool: convergeFunction(func_name, reason) — stop optimizing this function."""
    ok = convergeFunction(func_name, reason)
    msg = f"Converged on {func_name}: {reason}" if ok else f"Function {func_name} not found in refinement state"
    if context is not None:
        context.append({"type": "tool_use", "tool": "convergeFunction",
                       "input": {"func_name": func_name, "reason": reason}, "output": msg})
    return msg


def toolAvoidStrategy(func_name, strategy, reason, context=None):
    """Tool: avoidStrategy(func_name, strategy, reason) — mark a strategy as known-bad."""
    ok = avoidStrategy(func_name, strategy, reason)
    msg = f"Strategy '{strategy}' marked as avoid for {func_name}: {reason}" if ok else f"Function {func_name} not found"
    if context is not None:
        context.append({"type": "tool_use", "tool": "avoidStrategy",
                       "input": {"func_name": func_name, "strategy": strategy, "reason": reason},
                       "output": msg})
    return msg


def toolGetRefinementState(func_name, context=None):
    """Tool: getRefinementState(func_name) — show refinement history for a function."""
    summary = getAttemptSummary(func_name)
    if context is not None:
        context.append({"type": "tool_use", "tool": "getRefinementState",
                       "input": func_name, "output": summary})
    return summary


def toolGetUntriedStrategies(func_name, context=None):
    """Tool: getUntriedStrategies(func_name) — list strategies not yet tried."""
    untried = getUntriedStrategies(func_name)
    output = f"Untried strategies for {func_name}: {', '.join(untried) if untried else 'none — all exhausted'}"
    if context is not None:
        context.append({"type": "tool_use", "tool": "getUntriedStrategies",
                       "input": func_name, "output": output})
    return output


def toolAddSubComponent(func_name, sub_name, description, context=None):
    """Tool: addSubComponent(func_name, sub_name, description) — register a sub-component."""
    ok = addSubComponent(func_name, sub_name, description)
    msg = f"Added sub-component '{sub_name}' to {func_name}" if ok else f"Function {func_name} not found"
    if context is not None:
        context.append({"type": "tool_use", "tool": "addSubComponent",
                       "input": {"func_name": func_name, "sub_name": sub_name},
                       "output": msg})
    return msg
