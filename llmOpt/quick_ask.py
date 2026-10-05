#!/usr/bin/env python3
"""quick_ask - a cheap, stateless judgment primitive for the gengin optimizer.

The main optimizer model calls this as an MCP tool for narrow judgments over
text it provides: classification, relevance, yes/no questions. It knows
nothing about PRs, the repo, or the session; the caller passes the question
and context and gets an answer back.

The tool answers through the TypeSafe Jev decision model (System One) over
the OpenRouter Decisions API: typed answers with calibrated probabilities
instead of generated text. There is deliberately no chat backend - this is
not a text generator, it is a fast, cheap classifier that is allowed to be
wrong and is meant to be called in bulk; the main model decides what to do
with the verdicts.

Modes (pass exactly one):
  choices=[...]   -> {"choice": <option>, "confidence"?: float,
                      "probabilities"?: {option: float}}
                     (a Jev choice question over the caller's labels)
  yes_no=True     -> {"noul": <P(yes), 0..1>}
  questions={...} -> {"answers": {name: <typed answer>}}
                     (noul | choice | score questions answered in parallel
                      in one request over the shared context)

Design rules:
  - the Decisions API is used directly (QUICK_ASK_DECISIONS_URL, default
    https://openrouter.ai/api/alpha/decisions - outside /api/v1); the chat
    proxy's quantization/floor filters do not apply to Jev and are bypassed;
  - context is capped at QUICK_ASK_MAX_CONTEXT_CHARS (truncated, logged);
    questions and battery instructions are capped at
    QUICK_ASK_MAX_QUESTION_CHARS (rejected, never truncated); a battery holds
    at most QUICK_ASK_MAX_QUESTIONS questions;
  - a wall-clock deadline (QUICK_ASK_TIMEOUT_SECONDS) bounds every request;
  - every answer is validated locally (choice in criteria, noul/confidence in
    [0, 1], probabilities numeric) before it is returned;
  - failures return {"error": ...} - nothing raises into the caller;
  - calls that reach the cache or the network are logged as JSONL, cache
    hits included, so verdicts can be audited after a session;
  - advisory only: the main model sees the verdict and decides.

Standard library only, like openrouter_keys.py. Authentication uses
OPENROUTER_API_KEY from the environment (the per-session capped key the
supervisor injects); the key is never logged.
"""

import copy
import hashlib
import json
import os
import threading
import time
import urllib.error
import urllib.request

_llmOpt_dir = os.path.dirname(os.path.abspath(__file__))

# Model override for the decision model; defaults to the pinned Jev release.
MODEL = os.environ.get("QUICK_ASK_MODEL", "").strip() or "typesafe/jev-1.13"

# The Decisions API lives outside the /api/v1 prefix the chat client uses;
# requests go there directly (the chat proxy filters do not apply to Jev).
DECISIONS_URL = os.environ.get(
    "QUICK_ASK_DECISIONS_URL",
    "https://openrouter.ai/api/alpha/decisions").strip()

TIMEOUT_SECONDS = float(os.environ.get("QUICK_ASK_TIMEOUT_SECONDS", "30") or 30)
MAX_CONTEXT_CHARS = int(
    os.environ.get("QUICK_ASK_MAX_CONTEXT_CHARS", "24000") or 24000)
MAX_QUESTION_CHARS = int(
    os.environ.get("QUICK_ASK_MAX_QUESTION_CHARS", "8000") or 8000)
MAX_QUESTIONS = int(os.environ.get("QUICK_ASK_MAX_QUESTIONS", "24") or 24)

LOG_PATH = os.environ.get("QUICK_ASK_LOG", os.path.join(
    _llmOpt_dir, "logs", "quick_ask.jsonl"))
LOG_MAX_FIELD = 4000  # per-field truncation in the audit log

# HTTP codes OpenRouter documents as retryable.
_TRANSIENT_CODES = frozenset((429, 500, 502, 503, 524, 529))

_cache = {}  # input hash -> response payload (dict)


def _error(message):
    return {"error": str(message)}


def _cache_key(url, payload):
    canonical = json.dumps(
        {"url": url, "payload": payload}, sort_keys=True,
        separators=(",", ":"))
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


# --- argument validation ----------------------------------------------------

def _validate_choices(choices):
    if not isinstance(choices, (list, tuple)) or len(choices) < 2:
        return None, "choices must be a list of at least 2 non-empty strings"
    cleaned = []
    for choice in choices:
        if not isinstance(choice, str) or not choice.strip():
            return None, "choices must be a list of at least 2 non-empty strings"
        cleaned.append(choice.strip())
    if len(set(cleaned)) != len(cleaned):
        return None, "choices must not contain duplicates"
    return cleaned, None


def _validate_questions(questions):
    if not isinstance(questions, dict) or not questions:
        return None, "questions must be a non-empty object of named questions"
    if len(questions) > MAX_QUESTIONS:
        return None, f"questions must contain at most {MAX_QUESTIONS} entries"
    cleaned = {}
    for name, spec in questions.items():
        if not isinstance(name, str) or not name.strip():
            return None, "question names must be non-empty strings"
        if not isinstance(spec, dict):
            return None, f"question {name!r} must be an object"
        question_type = spec.get("type")
        if question_type not in ("noul", "choice", "score"):
            return None, (
                f"question {name!r}: type must be noul, choice or score")
        instructions = spec.get("instructions")
        if not isinstance(instructions, str) or not instructions.strip():
            return None, (
                f"question {name!r}: instructions must be a non-empty string")
        if len(instructions) > MAX_QUESTION_CHARS:
            return None, (
                f"question {name!r}: instructions exceed "
                f"{MAX_QUESTION_CHARS} characters")
        criteria = spec.get("criteria")
        entry = {"type": question_type, "instructions": instructions.strip()}
        if question_type == "choice":
            if not isinstance(criteria, dict) or len(criteria) < 2:
                return None, (
                    f"question {name!r}: choice criteria must be an object "
                    "with at least 2 labels")
            for label in criteria:
                if not isinstance(label, str) or not label.strip():
                    return None, (
                        f"question {name!r}: choice labels must be "
                        "non-empty strings")
            entry["criteria"] = criteria
        elif question_type == "score":
            if not isinstance(criteria, list) or len(criteria) < 1:
                return None, (
                    f"question {name!r}: score criteria must be a non-empty list")
            entry["criteria"] = criteria
        elif criteria is not None:
            if not isinstance(criteria, dict) or "true" not in criteria \
                    or "false" not in criteria:
                return None, (
                    f"question {name!r}: noul criteria must be an object with "
                    "'true' and 'false'")
            entry["criteria"] = criteria
        cleaned[name] = entry
    return cleaned, None


def _is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _is_probability(value):
    return _is_number(value) and 0.0 <= float(value) <= 1.0


# --- HTTP -------------------------------------------------------------------

def _is_in_flight_budget(raw_body):
    try:
        data = json.loads(raw_body.decode("utf-8", "ignore"))
    except (ValueError, AttributeError):
        return False
    if not isinstance(data, dict):
        return False
    error = data.get("error")
    if not isinstance(error, dict):
        return False
    metadata = error.get("metadata")
    return isinstance(metadata, dict) and \
        metadata.get("limit_source") == "openrouter_in_flight_budget"


def _http_error_message(code, raw_body):
    if isinstance(raw_body, bytes):
        snippet = raw_body.decode("utf-8", "ignore")[:500]
    else:
        snippet = str(raw_body)[:500]
    snippet = snippet or "(no body)"
    if code == 402:
        if _is_in_flight_budget(raw_body):
            return f"openrouter returned 402 (transient: in-flight budget): {snippet}"
        return f"openrouter returned 402 (terminal: credits or key limit): {snippet}"
    if code in _TRANSIENT_CODES:
        return f"openrouter returned {code} (transient): {snippet}"
    return f"openrouter returned {code}: {snippet}"


def _http_post(url, payload, timeout):
    """One POST returning (parsed, None) or (None, error message).

    Never logs or returns the Authorization header."""
    api_key = os.environ.get("OPENROUTER_API_KEY", "")
    if not api_key:
        return None, "OPENROUTER_API_KEY is not set in the environment"
    request = urllib.request.Request(
        url,
        data=json.dumps(payload).encode(),
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
            "User-Agent": "gengin-llmopt-quick-ask/1.0",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read(1_000_000)
    except urllib.error.HTTPError as exc:
        try:
            raw = exc.read(8192)
        except OSError:
            raw = b""
        try:
            exc.close()
        except OSError:
            pass
        return None, _http_error_message(exc.code, raw)
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        reason = getattr(exc, "reason", exc)
        return None, f"request failed: {reason}"
    except ValueError as exc:
        return None, f"request failed: {exc}"
    try:
        return json.loads(body), None
    except (json.JSONDecodeError, UnicodeDecodeError):
        return None, "openrouter returned non-JSON"


def _request_with_deadline(url, payload, timeout):
    """Enforce a wall-clock deadline around one request.

    urllib's timeout is per socket operation, so a server that drips bytes
    could keep a request alive past the deadline; the worker-thread join
    bounds the time the caller spends regardless."""
    box = {}

    def worker():
        try:
            box["result"] = _http_post(url, payload, timeout)
        except Exception as exc:  # defensive: never leak into the caller
            box["result"] = (None, f"request failed: {exc}")

    thread = threading.Thread(target=worker, daemon=True)
    thread.start()
    thread.join(timeout)
    if thread.is_alive():
        return None, f"request exceeded the {timeout:g}s deadline"
    return box.get("result", (None, "request failed: no result"))


def _normalize_usage(usage):
    if not isinstance(usage, dict):
        return None
    normalized = {}
    for source, target in (
            ("input_tokens", "input_tokens"),
            ("prompt_tokens", "input_tokens"),
            ("output_tokens", "output_tokens"),
            ("completion_tokens", "output_tokens")):
        value = usage.get(source)
        if _is_number(value):
            normalized[target] = value
    cost = usage.get("cost")
    if _is_number(cost):
        normalized["cost"] = cost
    return normalized or None


# --- request / response -----------------------------------------------------

def _build_request(mode, question, context, choices, questions):
    """Build (payload, expectations, error) for the Decisions API."""
    if mode == "questions":
        cleaned, err = _validate_questions(questions)
        if err:
            return None, None, err
    elif mode == "choices":
        cleaned = {"answer": {
            "type": "choice",
            "instructions": question.strip(),
            "criteria": {choice: choice for choice in choices},
        }}
    else:  # yes_no
        cleaned = {"answer": {
            "type": "noul",
            "instructions": question.strip(),
        }}
    payload = {
        "model": MODEL,
        "state": {"context": context},
        "questions": cleaned,
    }
    session_id = os.environ.get("GENGIN_SESSION_ID", "").strip()
    if session_id:
        payload["session_id"] = session_id[:256]
    return payload, cleaned, None


def _parse_response(parsed, expectations):
    """Validate and normalize a Decisions response into (answers, error)."""
    answers = parsed.get("answers") if isinstance(parsed, dict) else None
    if not isinstance(answers, dict):
        return None, "decisions response missing answers"
    if set(answers) != set(expectations):
        return None, "decisions answers do not match the requested questions"
    cleaned = {}
    for name, spec in expectations.items():
        answer = answers[name]
        if not isinstance(answer, dict):
            return None, f"decisions answer {name!r} is not an object"
        if answer.get("type") != spec["type"]:
            return None, f"decisions answer {name!r} has an unexpected type"
        if spec["type"] == "noul":
            value = answer.get("noul")
            if not _is_probability(value):
                return None, f"decisions answer {name!r} has an invalid noul value"
            cleaned[name] = {"noul": float(value)}
            continue
        if spec["type"] == "choice":
            choice = answer.get("choice")
            if choice not in spec["criteria"]:
                return None, (
                    f"decisions answer {name!r} returned choice {choice!r} "
                    f"not in {list(spec['criteria'])!r}")
            entry = {"choice": choice}
        else:  # score
            score = answer.get("score")
            if not _is_number(score):
                return None, f"decisions answer {name!r} has an invalid score"
            entry = {"score": float(score)}
        confidence = answer.get("confidence")
        if confidence is not None:
            if not _is_probability(confidence):
                return None, f"decisions answer {name!r} has an invalid confidence"
            entry["confidence"] = float(confidence)
        probabilities = answer.get("probabilities")
        if probabilities is not None:
            if not isinstance(probabilities, dict):
                return None, f"decisions answer {name!r} has invalid probabilities"
            for label, probability in probabilities.items():
                if spec["type"] == "choice" and label not in spec["criteria"]:
                    return None, (
                        f"decisions answer {name!r} has a probability for an "
                        "unknown label")
                if not _is_number(probability):
                    return None, (
                        f"decisions answer {name!r} has invalid probabilities")
            entry["probabilities"] = probabilities
        legend = answer.get("legend")
        if isinstance(legend, dict):
            entry["legend"] = legend
        cleaned[name] = entry
    return cleaned, None


def _build_log_entry(mode, question, questions, context, truncated, cache_key):
    entry = {
        "hash": cache_key,
        "model": MODEL,
        "mode": mode,
        "context_chars": len(context),
        "context_truncated": truncated,
    }
    if mode == "questions" and isinstance(questions, dict):
        entry["question_count"] = len(questions)
        entry["question_types"] = {
            name: spec.get("type") for name, spec in questions.items()
            if isinstance(spec, dict)
        }
    else:
        entry["question"] = question.strip()
    return entry


def quick_ask(question="", context="", choices=None, yes_no=False,
              questions=None):
    """Ask the decision model a narrow question. Always returns a dict:

      choices=[...]   -> {"choice": ..., "confidence"?, "probabilities"?}
      yes_no=True     -> {"noul": <P(yes), 0..1>}
      questions={...} -> {"answers": {name: {...}}}
      any failure     -> {"error": str}
    """
    selected = [name for name, active in (
        ("questions", questions is not None),
        ("choices", choices is not None),
        ("yes_no", bool(yes_no)),
    ) if active]
    if len(selected) > 1:
        return _error("pass exactly one of questions, choices or yes_no")
    if not selected:
        return _error(
            "pass one of questions, choices or yes_no; the tool only answers "
            "typed questions and never generates text")
    mode = selected[0]

    if mode == "questions":
        if isinstance(question, str) and question.strip():
            return _error("pass either question or questions, not both")
    else:
        if not isinstance(question, str) or not question.strip():
            return _error("question must be a non-empty string")
        if len(question) > MAX_QUESTION_CHARS:
            return _error(
                f"question exceeds the {MAX_QUESTION_CHARS}-character limit")

    if context is None:
        context = ""
    if not isinstance(context, str):
        return _error("context must be a string")
    truncated = len(context) > MAX_CONTEXT_CHARS
    if truncated:
        context = context[:MAX_CONTEXT_CHARS]

    if not DECISIONS_URL.startswith(("http://", "https://")):
        return _error("QUICK_ASK_DECISIONS_URL must be an absolute http(s) URL")

    if mode == "choices":
        choices, err = _validate_choices(choices)
        if err:
            return _error(err)

    payload, expectations, err = _build_request(
        mode, question, context, choices, questions)
    if err:
        return _error(err)

    cache_key = _cache_key(DECISIONS_URL, payload)
    log_entry = _build_log_entry(
        mode, question, questions, context, truncated, cache_key)
    if cache_key in _cache:
        cached = copy.deepcopy(_cache[cache_key])
        cached["cached"] = True
        log_entry["cached"] = True
        log_entry["ms"] = 0
        log_entry["response"] = json.dumps(cached)
        _log_call(log_entry)
        return cached

    started = time.monotonic()
    parsed, err = _request_with_deadline(DECISIONS_URL, payload, TIMEOUT_SECONDS)
    log_entry["ms"] = int((time.monotonic() - started) * 1000)
    if err:
        log_entry["error"] = err
        _log_call(log_entry)
        return _error(err)

    answers, err = _parse_response(parsed, expectations)
    if err:
        log_entry["error"] = err
        log_entry["raw"] = json.dumps(parsed)[:LOG_MAX_FIELD]
        _log_call(log_entry)
        return _error(err)
    if mode == "questions":
        result = {"answers": answers}
    else:
        result = answers["answer"]

    usage = _normalize_usage(
        parsed.get("usage") if isinstance(parsed, dict) else None)
    if usage:
        log_entry["usage"] = usage
    served_model = parsed.get("model") if isinstance(parsed, dict) else None
    if isinstance(served_model, str) and served_model:
        log_entry["served_model"] = served_model

    log_entry["response"] = json.dumps(result)
    _log_call(log_entry)
    _cache[cache_key] = copy.deepcopy(result)
    return copy.deepcopy(result)
