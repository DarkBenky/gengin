#!/usr/bin/env python3
"""delegate - one-shot delegation to the small local code model.

The main optimizer model calls this as an MCP tool to hand ONE narrow,
mechanical job to the small local Qwen2.5-Coder-7B instead of spending its
own turns on it.  This is a LOCAL-RUN-ONLY tool: it is enabled when the
launcher's local preset exports GENGIN_SMALL_MODEL_ENABLE=1, because the
small model runs on the same box as the local main model, so the call costs
no API money and little wall time.  On cloud presets the MCP server does not
register the tool at all.

It is a REALLY SMALL, DUMB model:
  - it cannot plan, search the repo, read files or call tools; it sees only
    the task and context the caller passes;
  - it is decent at small, fully specified text/code transformations (rename,
    reformat, extract fields, write one function body) and bad at anything
    that needs judgement;
  - its output is often subtly wrong, so the caller must always diff, compile
    or test it before using it.

Design rules (the quick_ask shape):
  - stateless: exactly one chat completion per call, no history;
  - the endpoint is the local llama-server that also serves FIM
    (GENGIN_SMALL_MODEL_BASE, default http://127.0.0.1:8030/v1); it is NOT the
    router, so a delegation never forces the big local model to swap in - but
    it does share the FIM slots, so keep requests short;
  - task is capped and rejected when too long, context is capped and truncated
    (both reported);
  - a wall-clock deadline (GENGIN_SMALL_MODEL_TIMEOUT_SECONDS) bounds the call;
  - failures return {"error": ...} - nothing raises into the caller;
  - calls are logged as JSONL for audit.

Standard library only, like quick_ask.py.
"""

import json
import os
import time
import urllib.error
import urllib.request

_llmOpt_dir = os.path.dirname(os.path.abspath(__file__))

BASE_URL = (os.environ.get("GENGIN_SMALL_MODEL_BASE", "").strip()
            or "http://127.0.0.1:8030/v1").rstrip("/")
# llama-server ignores the id; it only has to look like a model name.
MODEL = os.environ.get("GENGIN_SMALL_MODEL_NAME", "").strip() or "qwen2.5-coder-7b"

TIMEOUT_SECONDS = float(
    os.environ.get("GENGIN_SMALL_MODEL_TIMEOUT_SECONDS", "180") or 180)
MAX_TASK_CHARS = int(
    os.environ.get("GENGIN_SMALL_MODEL_MAX_TASK_CHARS", "4000") or 4000)
MAX_CONTEXT_CHARS = int(
    os.environ.get("GENGIN_SMALL_MODEL_MAX_CONTEXT_CHARS", "16000") or 16000)
DEFAULT_MAX_TOKENS = int(
    os.environ.get("GENGIN_SMALL_MODEL_MAX_TOKENS", "600") or 600)
MAX_TOKENS_CAP = 2000

LOG_PATH = os.environ.get("GENGIN_SMALL_MODEL_LOG", os.path.join(
    _llmOpt_dir, "logs", "small_model.jsonl"))
LOG_MAX_FIELD = 4000

SYSTEM_PROMPT = (
    "You are a small, fast helper model. Do exactly what the task asks and "
    "nothing else. Reply with the requested output only - no explanation, no "
    "preamble, no markdown fences. If the task is ambiguous or needs "
    "information that is not in the context, reply with exactly: "
    "INSUFFICIENT CONTEXT")

INSUFFICIENT_MARKER = "INSUFFICIENT CONTEXT"


def is_enabled():
    """Local runs only: the launcher's local preset exports the flag."""
    return os.environ.get("GENGIN_SMALL_MODEL_ENABLE", "").strip() == "1"


def _error(message, **extra):
    payload = {"error": str(message)}
    payload.update(extra)
    return payload


def _log_call(entry):
    """Append one JSONL audit line; logging failures never break the tool."""
    try:
        os.makedirs(os.path.dirname(LOG_PATH), exist_ok=True)
        safe = dict(entry)
        for key, value in list(safe.items()):
            if isinstance(value, str) and len(value) > LOG_MAX_FIELD:
                safe[key] = value[:LOG_MAX_FIELD] + "...<truncated>"
        safe["ts"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        with open(LOG_PATH, "a") as fh:
            fh.write(json.dumps(safe, separators=(",", ":")) + "\n")
    except OSError:
        pass


def _timeout_error(task):
    _log_call({"event": "timeout", "task": task,
               "timeout_s": TIMEOUT_SECONDS})
    return _error(
        f"small model timed out after {TIMEOUT_SECONDS:g}s; shorten the task "
        "or raise GENGIN_SMALL_MODEL_TIMEOUT_SECONDS")


def _user_message(task, context):
    """Task first, then the raw input in obvious delimiters."""
    if not context:
        return task
    return task + "\n\nContext:\n<<<\n" + context + "\n>>>"


def delegate(task, context="", max_tokens=0):
    """Run one task against the small local model and return its text."""
    if not is_enabled():
        return _error(
            "small-model delegation is enabled only for local runs "
            "(GENGIN_SMALL_MODEL_ENABLE=1, exported by `gengin-opt.sh local`); "
            "do the work yourself")
    task = (task or "").strip()
    context = context or ""
    if not task:
        return _error("task is required")
    if len(task) > MAX_TASK_CHARS:
        return _error(
            f"task is {len(task)} chars, limit is {MAX_TASK_CHARS}; shorten "
            "the instruction and put the input text in context")

    try:
        tokens = int(max_tokens or 0) or DEFAULT_MAX_TOKENS
    except (TypeError, ValueError):
        return _error(f"max_tokens must be an integer, got {max_tokens!r}")
    if not 1 <= tokens <= MAX_TOKENS_CAP:
        return _error(f"max_tokens must be 1..{MAX_TOKENS_CAP}, got {tokens}")

    context_truncated = len(context) > MAX_CONTEXT_CHARS
    if context_truncated:
        context = context[:MAX_CONTEXT_CHARS]

    body = {
        "model": MODEL,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": _user_message(task, context)},
        ],
        "max_tokens": tokens,
        "temperature": 0.1,
        "stream": False,
    }
    started = time.time()
    try:
        request = urllib.request.Request(
            BASE_URL + "/chat/completions",
            data=json.dumps(body).encode("utf-8"),
            headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(request, timeout=TIMEOUT_SECONDS) as response:
            raw = response.read()
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")[:500]
        _log_call({"event": "http_error", "status": exc.code, "task": task})
        return _error(f"small model HTTP {exc.code}: {detail}")
    except urllib.error.URLError as exc:
        if isinstance(exc.reason, TimeoutError):
            return _timeout_error(task)
        _log_call({"event": "unreachable", "task": task, "detail": str(exc)})
        return _error(
            f"small model not reachable at {BASE_URL} ({exc}). On this machine "
            "it is the FIM server: systemctl --user start llama-server-zeta")
    except TimeoutError:
        return _timeout_error(task)
    except OSError as exc:
        _log_call({"event": "unreachable", "task": task, "detail": str(exc)})
        return _error(
            f"small model not reachable at {BASE_URL} ({exc}). On this machine "
            "it is the FIM server: systemctl --user start llama-server-zeta")

    try:
        payload = json.loads(raw.decode("utf-8", "replace"))
        choice = payload["choices"][0]
        output = choice["message"]["content"] or ""
    except (KeyError, IndexError, TypeError, ValueError):
        text = raw.decode("utf-8", "replace")[:500]
        _log_call({"event": "bad_response", "task": task, "detail": text})
        return _error("unexpected response from the small model", response=text)

    elapsed_ms = int((time.time() - started) * 1000)
    finish_reason = choice.get("finish_reason", "")
    result = {"output": output, "finish_reason": finish_reason, "ms": elapsed_ms}
    usage = payload.get("usage") or {}
    if usage:
        result["tokens"] = {
            "prompt": usage.get("prompt_tokens"),
            "completion": usage.get("completion_tokens"),
        }
    if context_truncated:
        result["context_truncated"] = True
    if finish_reason == "length":
        result["truncated"] = True
    if output.strip() == INSUFFICIENT_MARKER:
        result["insufficient_context"] = True

    _log_call({"event": "ok", "task": task, "output": output,
               "ms": elapsed_ms, "finish_reason": finish_reason})
    return result
