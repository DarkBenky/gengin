#!/usr/bin/env python3
"""quick_ask - a cheap, stateless LLM primitive for the gengin optimizer.

The main optimizer model calls this as an MCP tool for narrow judgments over
text it provides: classification, yes/no, extraction.  It knows nothing about
PRs, the repo, or the session; the caller passes the question and context and
gets an answer back.

Design rules (see the matching MCP tool docstring):

  - Output is enforced with OpenRouter `response_format` (strict JSON schema)
    for `choices`/`json_schema` calls; `temperature=0` everywhere.
  - Quantization is pinned to fp8 or higher via `provider.quantizations`; the
    local filtering proxy (llmOpt/proxy) adds `:floor` and `allow_fallbacks:
    false` on top.  Base URL defaults to that proxy.
  - Context is capped (QUICK_ASK_MAX_CONTEXT_CHARS), a hard timeout applies,
    and failures return {"error": ...} - nothing raises into the caller.
  - Calls are cached by input hash and logged as JSONL so wrong verdicts from
    the cheap model can be audited after a session.
  - Advisory only: the main model sees the verdict and decides.

Standard library only, like openrouter_keys.py.  Authentication uses
OPENROUTER_API_KEY from the environment (the per-session capped key the
supervisor injects); the key is never logged.
"""

import hashlib
import json
import os
import time
import urllib.error
import urllib.request

_llmOpt_dir = os.path.dirname(os.path.abspath(__file__))

# Route through the local filtering proxy by default (quantization allowlist,
# :floor routing, bounded retries).  Overridable for manual use.
BASE_URL = os.environ.get(
    "QUICK_ASK_BASE_URL", "http://127.0.0.1:8787/api/v1").rstrip("/")

# Cheap, fast model.  Defaults to the session model only as a fallback so a
# bare manual setup still works; supervised deployments should set
# QUICK_ASK_MODEL to a genuinely cheap model.
MODEL = os.environ.get(
    "QUICK_ASK_MODEL",
    os.environ.get("OPENROUTER_MODEL", "deepseek/deepseek-v4-flash-0731"))

TIMEOUT_SECONDS = float(os.environ.get("QUICK_ASK_TIMEOUT_SECONDS", "30") or 30)
MAX_CONTEXT_CHARS = int(os.environ.get("QUICK_ASK_MAX_CONTEXT_CHARS", "24000") or 24000)
MAX_FREE_TOKENS = int(os.environ.get("QUICK_ASK_MAX_TOKENS", "150") or 150)
# fp8 or higher: q4/q3/q2 endpoints are never eligible.
QUANTIZATIONS = [
    q.strip()
    for q in os.environ.get("QUICK_ASK_QUANTIZATIONS", "fp8,fp16,bf16,fp32").split(",")
    if q.strip()
]

LOG_PATH = os.environ.get("QUICK_ASK_LOG", os.path.join(
    _llmOpt_dir, "logs", "quick_ask.jsonl"))
LOG_MAX_FIELD = 4000  # per-field truncation in the audit log

_cache = {}  # input hash -> response payload (dict)


def _error(message):
    return {"error": str(message)}


def _cache_key(payload):
    canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canonical.encode("utf-8", "ignore")).hexdigest()


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


def _build_response_format(choices, json_schema):
    """Strict JSON schema for the two structured modes."""
    if choices:
        schema = {
            "type": "object",
            "properties": {
                "choice": {"type": "string", "enum": list(choices)},
                "reason": {"type": "string",
                           "description": "one or two sentences, max"},
            },
            "required": ["choice", "reason"],
            "additionalProperties": False,
        }
        name = "quick_ask_choice"
    else:
        schema = dict(json_schema)
        schema.setdefault("type", "object")
        schema["additionalProperties"] = False
        name = "quick_ask_extract"
    return {
        "type": "json_schema",
        "json_schema": {"name": name, "strict": True, "schema": schema},
    }


def _chat(payload, timeout):
    """One POST to {BASE_URL}/chat/completions.  Returns (parsed, None) or
    (None, error_message).  Never logs or returns the Authorization header."""
    api_key = os.environ.get("OPENROUTER_API_KEY", "")
    if not api_key:
        return None, "OPENROUTER_API_KEY is not set in the environment"
    req = urllib.request.Request(
        f"{BASE_URL}/chat/completions",
        data=json.dumps(payload).encode(),
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
            "User-Agent": "gengin-llmopt-quick-ask/1.0",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read(1_000_000)), None
    except urllib.error.HTTPError as e:
        body = ""
        try:
            body = e.read(8192).decode("utf-8", "ignore")[:500]
        except OSError:
            pass
        return None, f"openrouter returned {e.code}: {body or '(no body)'}"
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        reason = getattr(e, "reason", e)
        return None, f"request failed: {reason}"
    except (json.JSONDecodeError, UnicodeDecodeError):
        return None, "openrouter returned non-JSON"


def quick_ask(question, context="", choices=None, json_schema=None):
    """Ask the cheap model a narrow question.  Always returns a dict:

      choices set     -> {"choice": <one of choices>, "reason": str}
      json_schema set -> {"result": <object matching the schema>}
      neither         -> {"answer": <short free text>}
      any failure     -> {"error": str}
    """
    if not isinstance(question, str) or not question.strip():
        return _error("question must be a non-empty string")
    if choices is not None:
        if (not isinstance(choices, (list, tuple)) or len(choices) < 2
                or not all(isinstance(c, str) and c.strip() for c in choices)):
            return _error("choices must be a list of at least 2 non-empty strings")
        choices = [c.strip() for c in choices]
    if json_schema is not None and not isinstance(json_schema, dict):
        return _error("json_schema must be an object (a JSON schema)")
    if choices and json_schema:
        return _error("pass choices or json_schema, not both")

    context = context or ""
    if not isinstance(context, str):
        return _error("context must be a string")
    truncated = False
    if len(context) > MAX_CONTEXT_CHARS:
        context = context[:MAX_CONTEXT_CHARS]
        truncated = True

    messages = [{
        "role": "system",
        "content": (
            "You are a fast, narrow judgment primitive inside a code-optimization "
            "agent. Answer only the question asked, only from the provided context "
            "and the question. No multi-step reasoning, no code generation. Be terse."
        ),
    }, {
        "role": "user",
        "content": (("Context:\n" + context + "\n\n") if context else "")
                   + "Question: " + question.strip(),
    }]

    payload = {
        "model": MODEL,
        "messages": messages,
        "temperature": 0,
        "max_tokens": MAX_FREE_TOKENS,
        "provider": {"quantizations": QUANTIZATIONS},
    }
    if choices or json_schema:
        payload["response_format"] = _build_response_format(choices, json_schema)
        payload["max_tokens"] = max(MAX_FREE_TOKENS, 512)

    key = _cache_key(payload)
    if key in _cache:
        cached = dict(_cache[key])
        cached["cached"] = True
        return cached

    started = time.monotonic()
    parsed, err = _chat(payload, timeout=TIMEOUT_SECONDS)
    elapsed_ms = int((time.monotonic() - started) * 1000)

    log_entry = {
        "hash": key, "model": MODEL, "question": question.strip(),
        "context_chars": len(context), "context_truncated": truncated,
        "mode": "choices" if choices else ("json_schema" if json_schema else "free"),
        "ms": elapsed_ms,
    }

    if err:
        log_entry["error"] = err
        _log_call(log_entry)
        return _error(err)

    try:
        message = parsed["choices"][0]["message"]
        content = (message.get("content") or "").strip()
    except (KeyError, IndexError, TypeError):
        log_entry["error"] = "malformed response"
        _log_call(log_entry)
        return _error("openrouter response missing choices[0].message.content")

    usage = parsed.get("usage") if isinstance(parsed, dict) else None
    if isinstance(usage, dict):
        log_entry["usage"] = {
            k: usage.get(k) for k in ("prompt_tokens", "completion_tokens")
            if k in usage
        }

    if not content:
        log_entry["error"] = "empty completion"
        _log_call(log_entry)
        return _error("model returned an empty completion")

    if choices or json_schema:
        try:
            data = json.loads(content)
        except (json.JSONDecodeError, UnicodeDecodeError):
            # response_format should guarantee JSON; treat a violation as an
            # error rather than guessing.
            log_entry["error"] = "non-JSON structured response"
            log_entry["raw"] = content
            _log_call(log_entry)
            return _error("model did not return valid JSON despite response_format")
        if choices:
            choice = data.get("choice")
            if choice not in choices:
                log_entry["error"] = "choice outside enum"
                log_entry["raw"] = content
                _log_call(log_entry)
                return _error(f"model returned choice {choice!r} not in {choices!r}")
            result = {"choice": choice, "reason": str(data.get("reason", ""))}
        else:
            result = {"result": data}
    else:
        result = {"answer": content}

    log_entry["response"] = json.dumps(result)
    _log_call(log_entry)
    _cache[key] = result
    return dict(result)
