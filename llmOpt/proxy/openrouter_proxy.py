#!/usr/bin/env python3
"""OpenRouter filtering proxy for the gengin optimizer.

Sits between Hermes (and any other local tool) and OpenRouter and makes the
cheap-provider problem impossible by injection, not by trust:

  - adds `provider.quantizations` (default: int8/fp6/fp8/fp16/bf16/fp32/unknown)
    so low-quantization (q4/q3/q2) endpoints are not silently selected;
  - adds `provider.allow_fallbacks: false` by default, so OpenRouter does not
    walk through provider fallbacks on a failure; the proxy itself retries
    exactly once without any injection, then backs off per model;
  - appends `:floor` (price-sorted routing) to the model id unless the caller
    already chose a routing variant;
  - translates a provider pin in the model id (``model:deepseek``,
    ``model:xiaomi/fp8``) into `provider.order`, because OpenRouter only
    documents a fixed slug variant set (`:nitro`, `:floor`, `:free`, ...) and
    silently *ignores* anything else - an untranslated pin looks harmless and
    still gets a 200, but the request is load-balanced by price across all
    providers, which is exactly what the pin was meant to prevent.

Skip rules - the request is passed through untouched when the caller clearly
made an explicit choice:

  - model id carries ``:free`` (already the cheapest cluster; a quantization
    filter would exclude its endpoints);
  - the request pins providers itself (``provider.only`` / ``provider.order``);
  - the body has no single string ``model`` (multi-model fallback arrays).

Give-up coach (on by default): the model's own answer text is scanned for
wording that reads like it is about to stop early, and for streaks of rejected
candidates.  On a trigger, one supervisor reminder plus 1-2 technique hints
(picked from hints.txt by the family the reported counters point at) is queued
and injected as an extra user message into the *next* request of the same
conversation - rate-limited per conversation and never repeating a hint.  A
response that already carries a `report_session_result` tool call is skipped:
that turn ends the session.  Decisions are logged as `coach key=... ...`.

Context-aware route (registered at runtime, off by default): a launcher
(`gengin-opt.sh`, `supervisor.py`) can install a two-model chain over loopback
(`POST /route/register`), e.g. Claude Haiku -> DeepSeek.  Requests are served
by the first member until the conversation's prompt is estimated to cross a
token threshold, then that conversation switches to the second for good.  A
member is either an OpenRouter model id, or `{model, upstream: deepseek}` for
the DIRECT DeepSeek API (its own upstream, keyed by the registration's
`deepseekKey`; OpenRouter-only filters are skipped for that leg).  That leg
keeps thinking mode on: the proxy remembers the `reasoning_content` of every
response it served, replays it into the conversation's assistant messages
(thinking mode 400s tool requests whose replayed messages lost the field -
agent harnesses drop it), and backfills a " " pad over messages that
predate the proxy; `stream_options.include_usage` keeps the counting working).  The count
comes from the response `usage` (always returned by OpenRouter; the final SSE
chunk when streaming) plus a conservative estimate of the text added since.
This targets price tiers (Haiku input costs 5x above a 100K prompt), not
context windows.  Registration lives in proxy memory, expires after its TTL,
and is inspectable at `GET /route`; `POST /route/clear` removes it
(routeId-guarded).  The direct leg's cost is computed from DeepSeek's price
table (off-peak aware, llmOpt/deepseek_pricing.py); every served request lands
in a SQLite log with a dashboard at `GET /logs`; and a route can carry a
session budget (`budgetUsd`) that injects a warning at the configured
fraction and at 95%, and stops the conversation with HTTP 402 once spent.

Standard library only (http.server + urllib), streaming-safe: response bodies
are relayed with chunked transfer encoding as they arrive, so SSE completions
are not buffered. Never logs bodies, headers or keys.

Env knobs (all optional):
  GENGIN_PROXY_PORT=8787
  GENGIN_PROXY_BIND=127.0.0.1
  GENGIN_PROXY_UPSTREAM=https://openrouter.ai
  GENGIN_PROXY_QUANTIZATIONS=int8,fp6,fp8,fp16,bf16,fp32,unknown
  GENGIN_PROXY_FLOOR=1
  GENGIN_PROXY_ALLOW_FALLBACKS=0
  GENGIN_PROXY_PIN_FALLBACKS=0   # 0 = a provider pin is exclusive
  GENGIN_PROXY_FAIL_THRESHOLD=3
  GENGIN_PROXY_COOLDOWN_SECONDS=3600
  GENGIN_PROXY_TIMEOUT=300
  GENGIN_PROXY_COACH=1
  GENGIN_PROXY_COACH_MAX=3      # reminders per conversation
  GENGIN_PROXY_COACH_COOLDOWN=180
  GENGIN_PROXY_COACH_TEXT=...   # replace the default reminder text
  GENGIN_PROXY_HINTS_FILE=<proxy dir>/hints.txt
  GENGIN_PROXY_LOG=-            # "-" = stderr, or an absolute file path
  GENGIN_PROXY_ROUTE_CHARS_PER_TOKEN=3  # estimate divisor; lower = switch sooner
  GENGIN_PROXY_DEEPSEEK_UPSTREAM=https://api.deepseek.com
  GENGIN_PROXY_LOG_DB=<llmOpt>/state/requests.db  # "" or 0 disables the log
  GENGIN_PROXY_BUDGET_WARN=0.8         # session-budget warning fraction
  GENGIN_PROXY_BUDGET_TEXT=...         # replace the budget warning text
"""

import copy
import hashlib
import json
import os
import random
import re
import sqlite3
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import deepseek_pricing  # noqa: E402  (llmOpt/deepseek_pricing.py)

PORT = int(os.environ.get("GENGIN_PROXY_PORT", "8787") or 8787)
BIND = os.environ.get("GENGIN_PROXY_BIND", "127.0.0.1")
UPSTREAM = os.environ.get("GENGIN_PROXY_UPSTREAM", "https://openrouter.ai").rstrip("/")
# Second upstream for chain members tagged `{..., "upstream": "deepseek"}`.
DEEPSEEK_UPSTREAM = os.environ.get(
    "GENGIN_PROXY_DEEPSEEK_UPSTREAM", "https://api.deepseek.com").rstrip("/")
QUANTIZATIONS = [
    q.strip()
    for q in os.environ.get(
        "GENGIN_PROXY_QUANTIZATIONS", "int8,fp6,fp8,fp16,bf16,fp32,unknown"
    ).split(",")
    if q.strip()
]
ENABLE_FLOOR = os.environ.get("GENGIN_PROXY_FLOOR", "1") != "0"
# A pinned provider is exclusive by default: `order` alone would still let
# OpenRouter fall back to another provider when the pin is busy.
PIN_FALLBACKS = os.environ.get("GENGIN_PROXY_PIN_FALLBACKS", "0") != "0"
ALLOW_FALLBACKS = os.environ.get("GENGIN_PROXY_ALLOW_FALLBACKS", "0") != "0"
FAIL_THRESHOLD = int(os.environ.get("GENGIN_PROXY_FAIL_THRESHOLD", "3") or 3)
COOLDOWN_SECONDS = float(os.environ.get("GENGIN_PROXY_COOLDOWN_SECONDS", "3600") or 3600)
TIMEOUT = float(os.environ.get("GENGIN_PROXY_TIMEOUT", "300") or 300)
COACH_ENABLED = os.environ.get("GENGIN_PROXY_COACH", "1") != "0"
COACH_MAX = int(os.environ.get("GENGIN_PROXY_COACH_MAX", "3") or 3)
COACH_COOLDOWN = float(os.environ.get("GENGIN_PROXY_COACH_COOLDOWN", "180") or 180)
COACH_TEXT = os.environ.get("GENGIN_PROXY_COACH_TEXT") or (
    "[supervisor] Do not stop here: the budget is not spent and the `## Node map` in "
    "codebase_context.md is your candidate queue. Pick the next `untried` row (largest "
    "flame percent first) and measure it. A candidate that won >= 1% in run_func_bench "
    "must be applied and validated with make_bench before a no_change verdict is "
    "acceptable - applying and reverting is the normal loop."
)
LOG_TARGET = os.environ.get("GENGIN_PROXY_LOG", "-")
MAX_BODY = 64 * 1024 * 1024
# Route estimate divisor: characters per token, deliberately conservative
# (overestimating means the switch happens early, never after the price tier).
try:
    ROUTE_CHARS_PER_TOKEN = float(
        os.environ.get("GENGIN_PROXY_ROUTE_CHARS_PER_TOKEN", "3") or 3)
except ValueError:
    ROUTE_CHARS_PER_TOKEN = 3.0
if ROUTE_CHARS_PER_TOKEN <= 0:
    ROUTE_CHARS_PER_TOKEN = 3.0

# Request log (SQLite, served as a dashboard at GET /logs) and the session
# budget (warn at a fraction of budgetUsd, hard-stop at 100%).
LOG_DB = (os.environ.get("GENGIN_PROXY_LOG_DB")
          or os.path.join(os.path.dirname(os.path.dirname(
              os.path.abspath(__file__))), "state", "requests.db"))
if LOG_DB.strip().lower() in ("", "0", "off"):
    LOG_DB = ""
try:
    BUDGET_WARN_FRACTION = float(
        os.environ.get("GENGIN_PROXY_BUDGET_WARN", "0.8") or 0.8)
except ValueError:
    BUDGET_WARN_FRACTION = 0.8
if not 0.1 <= BUDGET_WARN_FRACTION <= 0.95:
    BUDGET_WARN_FRACTION = 0.8
BUDGET_TEXT = os.environ.get("GENGIN_PROXY_BUDGET_TEXT") or (
    "[budget] Session spend is ${spent:.2f} of the ${budget:.2f} cap "
    "({pct}%). Wrap up safely: finish the current measurement, record the "
    "results, and update codebase_context.md - the session stops when the "
    "cap is reached."
)

# OpenRouter slug variants: `:floor`/`:nitro`/`:free` and friends are
# documented. Any other suffix in a model id is a caller-chosen provider or
# endpoint pin (`:deepseek`, `:xiaomi/fp8`) - OpenRouter ignores unknown slug
# suffixes, so those must be translated into the request's provider object.
CATALOG_VARIANTS = {"free", "batch", "thinking", "extended"}
# `flex` is a service tier (cheaper, higher latency) rather than a routing
# variant, but it is a caller-chosen routing intent all the same: appending
# `:floor` on top of it is not what the caller asked for.
ROUTING_VARIANTS = {"nitro", "floor", "exacto", "online", "flex"}
KNOWN_VARIANTS = CATALOG_VARIANTS | ROUTING_VARIANTS

_lock = threading.Lock()
_model_state = {}  # model -> {"fails": int, "skip_until": float}
_stats = {
    "requests": 0, "injected": 0, "skipped": 0, "retries": 0, "failures": 0,
    "coach_detected": 0, "coach_injected": 0,
}

# "I am about to stop" and "that candidate failed" wording.  Patterns avoid
# backslashes so they stay readable in a diff.
GIVEUP_RE = re.compile(
    r"no[_ ]?change"
    r"|no (?:further|safe|remaining|more|other) (?:safe |measurable )?"
    r"(?:optimization|candidate|win|gain|idea|approach|experiment|variant|change)"
    r"|nothing (?:more|else|further) (?:left|to try|to do|worth)"
    r"|(?:will|shall|going to|let me) (?:now )?(?:stop|wrap up|conclude|end the session)"
    r"|(?:could not|couldn't|can(?:no|')t|cannot) find (?:any|a|another)"
    r"|out of (?:ideas|candidates|options)",
    re.IGNORECASE,
)
REJECT_RE = re.compile(
    r"(?:slower|regress(?:ed|ion)|no win|no gain|did not (?:improve|help)|"
    r"near noise|not worth|inconsistent)",
    re.IGNORECASE,
)
IPC_RE = re.compile(r"IPC[^0-9]{0,12}([0-9]+(?:[.][0-9]+)?)", re.IGNORECASE)
CACHE_RE = re.compile(r"cache[- ]miss[^0-9%]{0,24}([0-9]+(?:[.][0-9]+)?)[ ]*%", re.IGNORECASE)
BRANCH_RE = re.compile(r"branch[- ]miss[^0-9%]{0,24}([0-9]+(?:[.][0-9]+)?)[ ]*%", re.IGNORECASE)
REJECT_STREAK = 3
_coach_state = {}   # conversation key -> pending/count/last/used/streak/family
_hints_cache = {"mtime": 0.0, "items": []}
_route = None  # active context-aware route; see the route section below


def _coach_entry(key):
    return _coach_state.setdefault(key, {
        "pending": False, "count": 0, "last": 0.0, "seen": 0.0,
        "used": set(), "streak": 0, "family": "",
    })


def _load_hints():
    """(id, family, text) from hints.txt, re-read when the file changes."""
    path = os.environ.get("GENGIN_PROXY_HINTS_FILE") or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "hints.txt")
    try:
        mtime = os.path.getmtime(path)
    except OSError:
        return []
    with _lock:
        if mtime != _hints_cache["mtime"]:
            items = []
            try:
                with open(path) as fh:
                    for line in fh:
                        line = line.strip()
                        if not line or line.startswith("#"):
                            continue
                        parts = [part.strip() for part in line.split("|", 2)]
                        if len(parts) == 3 and all(parts):
                            items.append((parts[0], parts[1].lower(), parts[2]))
            except OSError:
                return []
            _hints_cache.update({"mtime": mtime, "items": items})
        return list(_hints_cache["items"])


def _hint_family(text):
    """The technique family the model's own counters point at."""
    ipc = IPC_RE.search(text)
    cache = CACHE_RE.search(text)
    branch = BRANCH_RE.search(text)
    if cache and float(cache.group(1)) >= 1.0:
        return "memory"
    if ipc and float(ipc.group(1)) < 0.7:
        return "memory"
    if branch and float(branch.group(1)) >= 5.0:
        return "branch"
    if ipc and float(ipc.group(1)) >= 1.5:
        return "compute"
    return ""


def _pick_hints(family, used, limit=2):
    hints = _load_hints()
    if not hints:
        return []
    chosen = []

    def take(pool):
        pool = [hint for hint in pool if hint[0] not in used and hint not in chosen]
        random.shuffle(pool)
        chosen.extend(pool[:max(0, limit - len(chosen))])

    if family:
        take([hint for hint in hints if hint[1] == family])
    if len(chosen) < limit:
        take([hint for hint in hints if hint[1] == "any"])
    if not chosen:
        take(hints)
    return chosen


def _log_line(line):
    stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    text = f"{stamp} {line}"
    if LOG_TARGET == "-":
        print(text, file=sys.stderr, flush=True)
        return
    try:
        with open(LOG_TARGET, "a") as fh:
            fh.write(text + "\n")
    except OSError:
        print(text, file=sys.stderr, flush=True)


def _model_status(model):
    with _lock:
        return dict(_model_state.get(model) or {"fails": 0, "skip_until": 0.0})


def _record_result(model, ok):
    with _lock:
        state = _model_state.setdefault(model, {"fails": 0, "skip_until": 0.0})
        if ok:
            state["fails"] = 0
            return
        state["fails"] += 1
        if state["fails"] >= FAIL_THRESHOLD:
            state["skip_until"] = time.time() + COOLDOWN_SECONDS
            state["fails"] = 0


def _conversation_key(body):
    """Digest of the session's system prompt: stable per session, nothing logged."""
    messages = body.get("messages")
    if not isinstance(messages, list):
        return ""
    for role in ("system", "user"):
        for message in messages:
            if not isinstance(message, dict) or message.get("role") != role:
                continue
            content = message.get("content")
            if isinstance(content, str) and content:
                return hashlib.sha1(content.encode("utf-8", "ignore")).hexdigest()[:12]
    return ""


class _ResponseTap:
    """Assistant text and tool-call fragments, streamed or not."""

    TAIL = 4000

    def __init__(self, streamed):
        self.streamed = streamed
        self.text = ""
        self.tools = ""
        self.usage = None
        self.reasoning = ""   # full thinking text, replayed into later requests
        self.content = ""     # assistant text, for the replay signature
        self._calls = {}      # tool-call index -> {id, name, args}
        self._raw = []
        self._buffer = ""
        self._finished = False

    def feed(self, chunk):
        if self.streamed:
            self._buffer += chunk.decode("utf-8", "ignore")
            while "\n" in self._buffer:
                line, self._buffer = self._buffer.split("\n", 1)
                self._feed_line(line)
        else:
            self._raw.append(chunk)

    def _feed_line(self, line):
        line = line.strip()
        if not line.startswith("data:"):
            return
        payload = line[5:].strip()
        if not payload or payload == "[DONE]":
            return
        try:
            data = json.loads(payload)
        except ValueError:
            return
        if isinstance(data.get("usage"), dict):
            self.usage = data["usage"]
        for choice in data.get("choices") or []:
            self._add(choice.get("delta") or choice.get("message") or {})

    def _add(self, delta):
        for field in ("content", "reasoning_content"):
            piece = delta.get(field)
            if isinstance(piece, str) and piece:
                self.text = (self.text + piece)[-self.TAIL:]
        piece = delta.get("content")
        if isinstance(piece, str) and piece:
            self.content = (self.content + piece)[-REPLAY_CONTENT_MAX:]
        piece = delta.get("reasoning_content")
        if isinstance(piece, str) and piece:
            self.reasoning = (self.reasoning + piece)[-REASONING_MAX_CHARS:]
        for call in delta.get("tool_calls") or []:
            if not isinstance(call, dict):
                continue
            fn = call.get("function")
            fn = fn if isinstance(fn, dict) else {}
            self.tools = (self.tools + str(fn.get("name") or "")
                          + str(fn.get("arguments") or ""))[-self.TAIL:]
            index = call.get("index")
            if not isinstance(index, int) or index < 0:
                index = len(self._calls)
            entry = self._calls.setdefault(index,
                                           {"id": "", "name": "", "args": ""})
            cid = call.get("id")
            if isinstance(cid, str) and cid:
                entry["id"] = cid
            if isinstance(fn.get("name"), str) and fn["name"]:
                entry["name"] = fn["name"]
            if isinstance(fn.get("arguments"), str) and fn["arguments"]:
                entry["args"] = (entry["args"]
                                 + fn["arguments"])[-REPLAY_CONTENT_MAX:]

    def finish(self):
        if self._finished:
            return self.text, self.tools
        self._finished = True
        if not self.streamed:
            try:
                data = json.loads(b"".join(self._raw).decode("utf-8", "ignore") or "{}")
            except ValueError:
                data = {}
            if isinstance(data.get("usage"), dict):
                self.usage = data["usage"]
            for choice in data.get("choices") or []:
                message = choice.get("message") or {}
                self._add({"content": message.get("content"),
                           "reasoning_content": message.get("reasoning_content"),
                           "tool_calls": message.get("tool_calls")})
        elif self._buffer.strip():
            # A final SSE line that arrived without its trailing newline still
            # carries counters worth keeping.
            self._feed_line(self._buffer)
            self._buffer = ""
        return self.text, self.tools

    def assistant_message(self):
        """The assistant message as a harness replays it back."""
        calls = [
            {"id": entry["id"] or None,
             "function": {"name": entry["name"], "arguments": entry["args"]}}
            for _, entry in sorted(self._calls.items())
        ]
        return {"role": "assistant", "content": self.content or None,
                "tool_calls": calls or None}


# --- reasoning replay (direct DeepSeek leg) --------------------------------
# Thinking mode rejects tool-carrying requests whose replayed assistant
# messages lost their `reasoning_content` (HTTP 400 "The reasoning_content in
# the thinking mode must be passed back to the API") - exactly what agent
# harnesses trigger by dropping the field.  The proxy remembers the reasoning
# of every response it served, keyed by the assistant message's replay
# signature, replays it on the next request, and backfills a " " pad for
# messages that predate the proxy (the API checks presence, not content).
REASONING_MAX_CHARS = 512 * 1024    # stored reasoning per response
REPLAY_CONTENT_MAX = 256 * 1024     # assistant text kept for signatures
REASONING_MAX_ENTRIES = 256         # per conversation
REASONING_MAX_CONVERSATIONS = 32
_reasoning_cache = {}  # conversation key -> {message signature: reasoning}


def _reasoning_signature(message):
    """Replay identity of an assistant message: tool-call ids when present
    (opaque and stable), canonical name+args otherwise, content hash last."""
    parts = []
    calls = message.get("tool_calls")
    if isinstance(calls, list):
        for call in calls:
            if not isinstance(call, dict):
                continue
            function = call.get("function")
            function = function if isinstance(function, dict) else {}
            call_id = call.get("id")
            if isinstance(call_id, str) and call_id:
                parts.append(call_id)
                continue
            arguments = function.get("arguments")
            if isinstance(arguments, str):
                try:
                    arguments = json.dumps(json.loads(arguments), sort_keys=True,
                                           separators=(",", ":"))
                except ValueError:
                    pass
            parts.append(f"{function.get('name') or ''}({arguments or ''})")
    if parts:
        return "tc:" + hashlib.sha1(
            "\x00".join(parts).encode("utf-8", "ignore")).hexdigest()
    content = message.get("content")
    if isinstance(content, list):
        try:
            content = json.dumps(content, sort_keys=True)
        except (TypeError, ValueError):
            return ""
    if not isinstance(content, str) or not content:
        return ""
    return "ct:" + hashlib.sha1(
        content.encode("utf-8", "ignore")).hexdigest()


def _reasoning_remember(conversation, message, reasoning):
    """Cache a served reasoning block under the message's replay signature."""
    signature = _reasoning_signature(message) if conversation else ""
    if not signature or not reasoning:
        return
    with _lock:
        entries = _reasoning_cache.pop(conversation, None)
        if entries is None:
            while len(_reasoning_cache) >= REASONING_MAX_CONVERSATIONS:
                _reasoning_cache.pop(next(iter(_reasoning_cache)))
            entries = {}
        _reasoning_cache[conversation] = entries
        entries.pop(signature, None)
        entries[signature] = reasoning[:REASONING_MAX_CHARS]
        while len(entries) > REASONING_MAX_ENTRIES:
            entries.pop(next(iter(entries)))


def _reasoning_replay(conversation, body):
    """Restore reasoning_content on the conversation's assistant messages:
    the cached value when this proxy served the message, a " " pad otherwise
    (the API requires the field, not its content).  Returns (cached,
    backfilled).
    """
    messages = body.get("messages")
    if not isinstance(messages, list):
        return 0, 0
    entries = {}
    if conversation:
        with _lock:
            entries = dict(_reasoning_cache.get(conversation) or {})
    cached = backfilled = 0
    for message in messages:
        if not isinstance(message, dict) or message.get("role") != "assistant":
            continue
        if message.get("reasoning_content"):
            continue
        signature = _reasoning_signature(message)
        if signature and signature in entries:
            message["reasoning_content"] = entries[signature]
            cached += 1
        else:
            message["reasoning_content"] = " "
            backfilled += 1
    return cached, backfilled


def _reasoning_observe(conversation, info, tap):
    """Remember the direct leg's reasoning for the next request's replay."""
    if info is None or info.get("upstream") != "deepseek" or tap is None:
        return
    tap.finish()
    if tap.reasoning:
        _reasoning_remember(conversation, tap.assistant_message(), tap.reasoning)


def _coach_observe(key, tap):
    """Queue a reminder on a give-up, or after a streak of rejected candidates."""
    text, tools = tap.finish()
    if not (COACH_ENABLED and key and text):
        return
    gave_up = GIVEUP_RE.search(text)
    with _lock:
        state = _coach_entry(key)
        if any(token in tools for token in ("patch", "make_bench", "create_pr")):
            state["streak"] = 0
        elif REJECT_RE.search(text):
            state["streak"] += 1
        streak = state["streak"]
    if not gave_up and streak < REJECT_STREAK:
        return
    reason = "giveup" if gave_up else "streak=%d" % streak
    with _lock:
        _stats["coach_detected"] += 1
    if "report_session_result" in tools:
        _log_line(f"coach key={key} detected={reason} final=1 queued=0")
        return
    family = _hint_family(text)
    with _lock:
        state = _coach_entry(key)
        state["pending"] = True
        state["family"] = family
        state["seen"] = time.time()
        state["streak"] = 0
    _log_line(f"coach key={key} detected={reason} family={family or '-'} queued=1")


def _coach_take(body):
    """Consume a queued reminder (+ technique hints) and return what to inject."""
    if not COACH_ENABLED:
        return ""
    key = _conversation_key(body)
    if not key:
        return ""
    now = time.time()
    with _lock:
        state = _coach_state.get(key)
        if not state or not state.get("pending"):
            return ""
        state["pending"] = False
        if state["count"] >= COACH_MAX or now - state["last"] < COACH_COOLDOWN:
            return ""
        state["count"] += 1
        state["last"] = now
        family, used = state.get("family", ""), state["used"]
        _stats["coach_injected"] += 1
        count = state["count"]
    hints = _pick_hints(family, used)
    with _lock:
        for hint in hints:
            used.add(hint[0])
    text = COACH_TEXT
    if hints:
        text += ("\n\nTechnique hints - hypotheses to prove in the bench, not "
                 "instructions; each still needs micro-bench -> apply -> make_bench:\n")
        text += "".join("- %s\n" % hint[2] for hint in hints)
    _log_line("coach key=%s injected=1 count=%d family=%s hints=%s"
              % (key, count, family or "-", ",".join(hint[0] for hint in hints) or "-"))
    return text


def _parse_model(model):
    """(base, variants, pins) for a model id.

    `variants` are documented OpenRouter slug variants.  `pins` are everything
    else the caller appended - provider or endpoint slugs (`deepseek`,
    `xiaomi/fp8`) that only work through the request's provider object.
    """
    parts = model.split(":")
    if len(parts) == 1:
        return model, [], []
    base = parts[0]
    suffixes = [s for s in parts[1:] if s]
    return (base,
            [s for s in suffixes if s in KNOWN_VARIANTS],
            [s for s in suffixes if s not in KNOWN_VARIANTS])


def _status_of(resp):
    """HTTP status for a urlopen result or an urllib HTTPError."""
    status = getattr(resp, "status", None)
    if status is None:
        status = getattr(resp, "code", None)
    return int(status or 0)


def _should_skip(body, model):
    if isinstance(body.get("models"), list) and body["models"]:
        return "multi-model"
    _, variants, _pins = _parse_model(model)
    if "free" in variants:
        return "free-variant"
    provider = body.get("provider")
    if isinstance(provider, dict):
        if provider.get("only") or provider.get("order"):
            return "provider-pinned"
    return ""


def _apply_filters(body, model, upstream="openrouter"):
    """Mutate body in place; return (applied_fields, skip_reason or "")."""
    if upstream != "openrouter":
        # Direct vendor upstreams get the request as-is - provider constraints
        # and :floor are OpenRouter-only; only the streamed-usage knob is set,
        # so the proxy can keep counting tokens on this leg (the reasoning
        # replay thinking mode requires happens in _reasoning_replay).
        if body.get("stream") is True and "stream_options" not in body:
            body["stream_options"] = {"include_usage": True}
            return ["stream_options"], ""
        return [], ""
    skip = _should_skip(body, model)
    if skip:
        return [], skip
    base, variants, pins = _parse_model(model)
    status = _model_status(model)
    if time.time() < status["skip_until"]:
        return [], "cooldown"

    applied = []
    provider = body.get("provider")
    provider = dict(provider) if isinstance(provider, dict) else {}
    if pins:
        # The pinned endpoint decides quantization, so neither the quantizations
        # allowlist nor `:floor` is added on top of a pin - `:floor` would also
        # make flex-tier endpoints eligible, i.e. the opposite of a pin.
        provider["order"] = list(pins)
        applied.append("pin:" + ",".join(pins))
        if not PIN_FALLBACKS and "allow_fallbacks" not in provider:
            provider["allow_fallbacks"] = False
            applied.append("allow_fallbacks")
    else:
        if not provider.get("quantizations"):
            provider["quantizations"] = list(QUANTIZATIONS)
            applied.append("quantizations")
        if not ALLOW_FALLBACKS and "allow_fallbacks" not in provider:
            provider["allow_fallbacks"] = False
            applied.append("allow_fallbacks")
    if provider:
        body["provider"] = provider

    outgoing = base + "".join(":" + v for v in variants)
    if ENABLE_FLOOR and not pins and not any(v in ROUTING_VARIANTS for v in variants):
        outgoing += ":floor"
        applied.append("floor")
    if outgoing != model:
        body["model"] = outgoing
    return applied, ""


# --- context-aware route ---------------------------------------------------
# A launcher registers a two-model chain over loopback: requests are served by
# the first member until the conversation's prompt is estimated to cross
# `maxContext` tokens, then by the second for the rest of that conversation.
# Counting uses OpenRouter's response `usage` (the only source, no extra key)
# plus a conservative estimate of what was added since; state is per
# conversation key and lives only as long as the registration.

ROUTE_MIN_CONTEXT = 1000
ROUTE_MAX_CONTEXT = 10000000
ROUTE_MAX_TTL = 604800
ROUTE_DEFAULT_TTL = 86400.0
ROUTE_SLUG_RE = re.compile(r"^[A-Za-z0-9._:/-]+$")


def _route_active():
    """The active route, dropping it once its TTL has elapsed."""
    global _route
    route = _route
    if (route is not None and route["expiresAt"] is not None
            and time.time() > route["expiresAt"]):
        with _lock:
            if _route is route:
                _route = None
        return None
    return route


def _normalize_member(member):
    """A route member: an OpenRouter model id, or {model, upstream} for a
    direct vendor API (currently only `deepseek`)."""
    if isinstance(member, str):
        if not ROUTE_SLUG_RE.fullmatch(member):
            return None
        return {"model": member, "upstream": "openrouter"}
    if isinstance(member, dict):
        model = member.get("model")
        upstream = member.get("upstream", "openrouter")
        if (not isinstance(model, str) or not ROUTE_SLUG_RE.fullmatch(model)
                or upstream not in ("openrouter", "deepseek")):
            return None
        return {"model": model, "upstream": upstream}
    return None


def _upstream_path(path, upstream):
    """Client path -> upstream path.  OpenRouter mirrors the client path; the
    DeepSeek API lives under /v1 while the client talks to /api/v1."""
    if upstream == "deepseek" and path.startswith("/api/v1"):
        return "/v1" + path[len("/api/v1"):]
    return path


def _route_register(payload):
    """Validate and install a route. Returns (http status, response dict)."""
    global _route
    if not isinstance(payload, dict):
        return 400, {"error": {"message": "route: expected a JSON object"}}
    members = payload.get("members")
    if not isinstance(members, list) or len(members) != 2:
        return 400, {"error": {"message":
            "route: members must be a list of two model ids"}}
    members = [_normalize_member(m) for m in members]
    if any(m is None for m in members):
        return 400, {"error": {"message":
            "route: every member must be a model id or {model, upstream}"}}
    bases = [_parse_model(m["model"])[0] for m in members]
    if bases[0] == bases[1]:
        return 400, {"error": {"message": "route: members must differ"}}
    max_context = payload.get("maxContext")
    if (isinstance(max_context, bool) or not isinstance(max_context, int)
            or not ROUTE_MIN_CONTEXT <= max_context <= ROUTE_MAX_CONTEXT):
        return 400, {"error": {"message":
            f"route: maxContext must be an integer {ROUTE_MIN_CONTEXT}..{ROUTE_MAX_CONTEXT}"}}
    ttl = payload.get("ttlSeconds", ROUTE_DEFAULT_TTL)
    if (isinstance(ttl, bool) or not isinstance(ttl, (int, float))
            or not 0 <= ttl <= ROUTE_MAX_TTL):
        return 400, {"error": {"message":
            f"route: ttlSeconds must be 0..{ROUTE_MAX_TTL}"}}
    label = payload.get("label", "")
    if not isinstance(label, str) or len(label) > 80:
        return 400, {"error": {"message": "route: label must be a short string"}}
    budget = payload.get("budgetUsd")
    if budget is not None and (isinstance(budget, bool)
                               or not isinstance(budget, (int, float))
                               or not 0 < budget <= 1000000):
        return 400, {"error": {"message":
            "route: budgetUsd must be a positive number"}}
    deepseek_key = payload.get("deepseekKey", "")
    if not isinstance(deepseek_key, str) or len(deepseek_key) > 512:
        return 400, {"error": {"message": "route: deepseekKey must be a short string"}}
    if (any(m["upstream"] == "deepseek" for m in members)
            and not deepseek_key and not os.environ.get("DEEPSEEK_API_KEY")):
        return 400, {"error": {"message":
            "route: a deepseek member needs deepseekKey (or DEEPSEEK_API_KEY "
            "in the proxy environment)"}}
    now = time.time()
    spec = {
        "routeId": os.urandom(8).hex(),
        "members": list(members),
        "bases": bases,
        "maxContext": int(max_context),
        "label": label,
        "registeredAt": now,
        "expiresAt": (now + float(ttl)) if ttl else None,
        "conversations": {},
        "counters": [{"requests": 0, "promptTokens": 0, "completionTokens": 0,
                      "costUsd": 0.0} for _ in members],
        "switches": 0,
        "deepseekKey": deepseek_key,
        "budgetUsd": float(budget) if budget is not None else None,
        "spentUsd": 0.0,
        "warnLevel": 0,
        "budgetPending": None,
    }
    with _lock:
        replaced = _route is not None
        _route = spec
    display = ",".join(
        m["model"] + ("@" + m["upstream"] if m["upstream"] != "openrouter" else "")
        for m in members)
    _log_line(f"route registered id={spec['routeId']} members={display} "
              f"maxContext={spec['maxContext']} label={label or '-'} "
              f"replaced={1 if replaced else 0}")
    return 200, {"routeId": spec["routeId"],
                 "members": [m["model"] for m in members],
                 "upstreams": [m["upstream"] for m in members],
                 "maxContext": spec["maxContext"],
                 "budgetUsd": spec["budgetUsd"],
                 "expiresAt": spec["expiresAt"]}


def _route_clear(payload):
    """Remove the active route when the caller's routeId matches."""
    global _route
    route_id = payload.get("routeId") if isinstance(payload, dict) else None
    with _lock:
        if _route is None:
            return 200, {"cleared": False, "detail": "no active route"}
        if not route_id or route_id != _route["routeId"]:
            return 409, {"cleared": False, "detail": "routeId mismatch"}
        cleared = _route["routeId"]
        _route = None
    _log_line(f"route cleared id={cleared}")
    return 200, {"cleared": True}


def _route_status():
    """Compact route state for /status and GET /route (never includes keys)."""
    route = _route_active()
    if route is None:
        return {"active": False}
    with _lock:
        conversations = [
            {"key": key, "member": state.get("member", 0),
             "lastPrompt": state.get("lastPrompt", 0),
             "switchedAt": state.get("switchedAt")}
            for key, state in list(route["conversations"].items())[-20:]
        ]
        return {
            "active": True,
            "routeId": route["routeId"],
            "members": [member["model"] for member in route["members"]],
            "upstreams": [member["upstream"] for member in route["members"]],
            "maxContext": route["maxContext"],
            "label": route["label"],
            "expiresAt": route["expiresAt"],
            "conversations": len(route["conversations"]),
            "switches": route["switches"],
            "budgetUsd": route.get("budgetUsd"),
            "spentUsd": round(route.get("spentUsd", 0.0), 6),
            "remainingUsd": (round(route["budgetUsd"] - route.get("spentUsd", 0.0), 6)
                             if route.get("budgetUsd") else None),
            "counters": [dict(counter) for counter in route["counters"]],
            "lastConversations": conversations,
        }


def _estimate_prompt(state, raw_len):
    """Prompt-token estimate: the last real usage plus the chars added since.

    ROUTE_CHARS_PER_TOKEN is deliberately low (overestimates), so a switch
    errs early - the opposite direction would cross the price tier first.
    """
    if state and state.get("lastPrompt"):
        delta = raw_len - state.get("lastChars", 0)
        if delta > 0:
            return state["lastPrompt"] + int(delta / ROUTE_CHARS_PER_TOKEN)
    return max(1, int(raw_len / ROUTE_CHARS_PER_TOKEN))


def _route_apply(body, model, raw_len, conversation):
    """Select the chain member for this request; rewrite body["model"] when
    the fallback is selected. Returns (model, info or None) - info carries the
    route reference so response usage lands on the spec that served it."""
    route = _route_active()
    if route is None:
        return model, None
    if _should_skip(body, model):
        # Requests the caller pinned (provider.only/order) or marked :free are
        # passed through untouched, so the route must not rewrite them either.
        return model, None
    base = _parse_model(model)[0]
    try:
        member = route["bases"].index(base)
    except ValueError:
        return model, None  # a request for an unrelated model - leave it alone

    switched = False
    with _lock:
        state = route["conversations"].get(conversation) if conversation else None
        estimate = _estimate_prompt(state, raw_len)
        sticky = bool(state and state.get("member") == 1)
        threshold_hit = estimate >= route["maxContext"]
        selected = 1 if (sticky or member == 1 or threshold_hit) else 0
        if conversation and state is None:
            state = {"member": 0, "lastPrompt": 0, "lastChars": 0}
            route["conversations"][conversation] = state
        if conversation and selected == 1 and state.get("member") != 1:
            # First time this conversation lands on the fallback - sticky from
            # here on. Only a threshold crossing counts (and logs) as a switch.
            state["member"] = 1
            state["switchedAt"] = time.time()
            if threshold_hit:
                route["switches"] += 1
                switched = True
    member_spec = route["members"][selected]
    if selected == 1 and body.get("model") != member_spec["model"]:
        model = member_spec["model"]
        body["model"] = model
    info = {"route": route, "member": selected, "est": estimate,
            "limit": route["maxContext"], "switched": switched,
            "upstream": member_spec["upstream"]}
    return model, info


def _response_cost(upstream, model, usage, when=None):
    """Cost of one response: provider-reported when present (OpenRouter sends
    `usage.cost`), computed from the price table for the direct DeepSeek leg."""
    cost = None
    if isinstance(usage, dict):
        raw = usage.get("cost")
        if isinstance(raw, (int, float)) and not isinstance(raw, bool):
            cost = float(raw)
    if cost is None and upstream == "deepseek":
        cost = deepseek_pricing.cost_usd(model, usage, when)
    return cost


def _budget_take(route):
    """Consume a queued budget warning (injected into the next request)."""
    with _lock:
        note = route.get("budgetPending")
        route["budgetPending"] = None
    return note or ""


def _budget_block(route):
    """Stop message when the session budget is exhausted, else ''."""
    budget = route.get("budgetUsd")
    spent = route.get("spentUsd", 0.0)
    if budget and spent >= budget:
        return (f"proxy: session budget exhausted (${spent:.2f} of "
                f"${budget:.2f} spent) - start a new session or raise the budget")
    return ""


def _route_observe(conversation, info, tap, raw_len):
    """Record the response's usage for the next request's estimate, add its
    cost to the session total, and queue a warning at the budget thresholds."""
    if info is None or tap is None:
        return
    usage = tap.usage
    if not isinstance(usage, dict):
        return
    route = info["route"]
    member = info["member"]
    prompt = usage.get("prompt_tokens")
    completion = usage.get("completion_tokens")
    cost_value = _response_cost(info.get("upstream"),
                                route["members"][member]["model"], usage)
    warning = ""
    with _lock:
        counters = route["counters"][member]
        counters["requests"] += 1
        if isinstance(prompt, int) and not isinstance(prompt, bool):
            counters["promptTokens"] += prompt
        if isinstance(completion, int) and not isinstance(completion, bool):
            counters["completionTokens"] += completion
        if cost_value is not None:
            counters["costUsd"] += cost_value
            route["spentUsd"] = route.get("spentUsd", 0.0) + cost_value
            budget = route.get("budgetUsd")
            if budget:
                spent = route["spentUsd"]
                level = (2 if spent >= budget * 0.95
                         else 1 if spent >= budget * BUDGET_WARN_FRACTION else 0)
                if level > route.get("warnLevel", 0):
                    route["warnLevel"] = level
                    route["budgetPending"] = BUDGET_TEXT.format(
                        spent=spent, budget=budget, pct=int(100 * spent / budget))
                    warning = (f"budget warn={level} spent={spent:.4f}"
                               f"/{budget:.4f}")
        if (conversation and isinstance(prompt, int)
                and not isinstance(prompt, bool) and prompt > 0):
            state = route["conversations"].get(conversation)
            if state is None:
                state = {"member": member, "lastPrompt": 0, "lastChars": 0}
                route["conversations"][conversation] = state
            state["lastPrompt"] = prompt
            state["lastChars"] = raw_len
    if warning:
        _log_line(warning)


# --- request log -----------------------------------------------------------
# One SQLite row per served request (tokens, cost, session, timing); served as
# a small dashboard at GET /logs - the OpenRouter logs panel equivalent for
# whatever runs through this proxy, including the direct DeepSeek leg.

_LOG_SCHEMA = (
    "CREATE TABLE IF NOT EXISTS requests (ts REAL, date TEXT, route_id TEXT,"
    " label TEXT, conv TEXT, model TEXT, upstream TEXT, stream INTEGER,"
    " status INTEGER, ms INTEGER, prompt INTEGER, completion INTEGER,"
    " cache_hit INTEGER, cache_miss INTEGER, cost REAL, retry INTEGER,"
    " flags TEXT)")
_LOG_COLUMNS = ("ts", "date", "route_id", "label", "conv", "model", "upstream",
                "stream", "status", "ms", "prompt", "completion", "cache_hit",
                "cache_miss", "cost", "retry", "flags")
_log_lock = threading.Lock()


def _usage_fields(usage):
    """(prompt, completion, cache_hit, cache_miss); ints or None per field."""
    def _int(value):
        return value if isinstance(value, int) and not isinstance(value, bool) else None
    if not isinstance(usage, dict):
        return None, None, None, None
    hit = _int(usage.get("prompt_cache_hit_tokens"))
    if hit is None:
        details = usage.get("prompt_tokens_details")
        hit = _int(details.get("cached_tokens")) if isinstance(details, dict) else None
    return (_int(usage.get("prompt_tokens")), _int(usage.get("completion_tokens")),
            hit, _int(usage.get("prompt_cache_miss_tokens")))


def _log_entry(model, upstream, route_info, conversation, status, ms, streamed,
               retried, applied, skip_reason, coach, budget, cached, backfilled,
               tap):
    """One log row for a served request; None when logging is disabled."""
    if not LOG_DB:
        return None
    usage = tap.usage if tap is not None else None
    prompt, completion, hit, miss = _usage_fields(usage)
    route = route_info["route"] if route_info else None
    flags = "+".join(part for part in (
        ("inject:" + "+".join(applied)) if applied else "",
        ("skip:" + skip_reason) if skip_reason else "",
        "coach" if coach else "",
        "budget" if budget else "",
        ("reasoning:%d+%d" % (cached, backfilled)) if (cached or backfilled) else "",
        "retry" if retried else "",
    ) if part)
    return {
        "ts": time.time(),
        "date": time.strftime("%Y-%m-%d %H:%M:%S"),
        "route_id": route["routeId"] if route else "",
        "label": route["label"] if route else "",
        "conv": conversation or "",
        "model": model,
        "upstream": upstream,
        "stream": 1 if streamed else 0,
        "status": int(status or 0),
        "ms": int(ms),
        "prompt": prompt,
        "completion": completion,
        "cache_hit": hit,
        "cache_miss": miss,
        "cost": _response_cost(upstream, model, usage),
        "retry": 1 if retried else 0,
        "flags": flags,
    }


def _log_request(entry):
    try:
        with _log_lock:
            connection = sqlite3.connect(LOG_DB, timeout=5)
            try:
                connection.execute(_LOG_SCHEMA)
                connection.execute(
                    "INSERT INTO requests (" + ", ".join(_LOG_COLUMNS) + ") VALUES ("
                    + ", ".join(":" + column for column in _LOG_COLUMNS) + ")",
                    entry)
                connection.commit()
            finally:
                connection.close()
    except sqlite3.Error as exc:
        _log_line(f"request log error: {type(exc).__name__}: {exc}")


def _logs_data(query):
    """Rows + totals for the /logs dashboard (query is a parse_qs dict)."""
    try:
        window = float((query.get("window") or ["86400"])[0])
    except ValueError:
        window = 86400.0
    try:
        limit = int((query.get("limit") or ["1000"])[0])
    except ValueError:
        limit = 1000
    limit = max(1, min(5000, limit))
    since = (time.time() - window) if window > 0 else 0.0
    response = {"rows": [], "totals": {"requests": 0, "cost": 0.0, "prompt": 0,
                                       "completion": 0, "cache_hit": 0,
                                       "cache_miss": 0}}
    if not LOG_DB:
        return response
    try:
        with _log_lock:
            connection = sqlite3.connect(LOG_DB, timeout=5)
            try:
                connection.execute(_LOG_SCHEMA)
                rows = connection.execute(
                    "SELECT " + ", ".join(_LOG_COLUMNS)
                    + " FROM requests WHERE ts >= ? ORDER BY ts DESC LIMIT ?",
                    (since, limit)).fetchall()
                totals = connection.execute(
                    "SELECT COUNT(*), COALESCE(SUM(cost), 0),"
                    " COALESCE(SUM(prompt), 0), COALESCE(SUM(completion), 0),"
                    " COALESCE(SUM(cache_hit), 0), COALESCE(SUM(cache_miss), 0)"
                    " FROM requests WHERE ts >= ?",
                    (since,)).fetchone()
            finally:
                connection.close()
        response["rows"] = [dict(zip(_LOG_COLUMNS, row)) for row in rows]
        response["totals"] = {"requests": totals[0], "cost": totals[1],
                              "prompt": totals[2], "completion": totals[3],
                              "cache_hit": totals[4], "cache_miss": totals[5]}
    except sqlite3.Error as exc:
        response["error"] = f"{type(exc).__name__}: {exc}"
    return response


def _cache_hit_pct(hit, miss):
    """Cache-hit rate over known prompt tokens, in percent; None when unknown."""
    denom = (hit or 0) + (miss or 0)
    if denom <= 0:
        return None
    return round(100.0 * (hit or 0) / denom, 2)


def _logs_export(query):
    """Full-fidelity CSV/JSON export of the request log (all matching rows).

    Query params mirror /logs/data: `window` (seconds, 0 = everything) and
    `format` (csv|json). Rows are oldest-first for analysis in a spreadsheet.
    Returns (filename, content_type, body).
    """
    import csv
    import io

    fmt = ((query.get("format") or ["csv"])[0]).strip().lower()
    if fmt not in ("csv", "json"):
        fmt = "csv"
    try:
        window = float((query.get("window") or ["0"])[0])
    except ValueError:
        window = 0.0
    since = (time.time() - window) if window > 0 else 0.0
    rows = []
    if LOG_DB:
        try:
            with _log_lock:
                connection = sqlite3.connect(LOG_DB, timeout=5)
                try:
                    connection.execute(_LOG_SCHEMA)
                    rows = connection.execute(
                        "SELECT " + ", ".join(_LOG_COLUMNS)
                        + " FROM requests WHERE ts >= ? ORDER BY ts ASC",
                        (since,)).fetchall()
                finally:
                    connection.close()
        except sqlite3.Error:
            rows = []
    stamp = time.strftime("%Y%m%d-%H%M%S")
    records = []
    for row in rows:
        record = dict(zip(_LOG_COLUMNS, row))
        record["timestamp_utc"] = time.strftime(
            "%Y-%m-%dT%H:%M:%SZ", time.gmtime(row[0]))
        record["cache_hit_pct"] = _cache_hit_pct(record.get("cache_hit"),
                                                 record.get("cache_miss"))
        records.append(record)
    if fmt == "json":
        payload = {
            "exportedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "windowSeconds": window,
            "rows": records,
        }
        body = json.dumps(payload, indent=2).encode() + b"\n"
        return f"gengin-requests-{stamp}.json", "application/json", body
    buffer = io.StringIO()
    writer = csv.writer(buffer)
    writer.writerow(("ts_epoch", "timestamp_utc", "date_local", "session",
                     "route_id", "conversation", "model", "upstream", "streamed",
                     "status", "ms", "prompt_tokens", "completion_tokens",
                     "cache_hit_tokens", "cache_miss_tokens", "cache_hit_pct",
                     "cost_usd", "retry", "flags"))
    for record in records:
        writer.writerow((
            record["ts"], record["timestamp_utc"], record["date"],
            record["label"] or "manual", record["route_id"], record["conv"],
            record["model"], record["upstream"], record["stream"],
            record["status"], record["ms"], record["prompt"],
            record["completion"], record["cache_hit"], record["cache_miss"],
            record["cache_hit_pct"], record["cost"], record["retry"],
            record["flags"]))
    return (f"gengin-requests-{stamp}.csv", "text/csv; charset=utf-8",
            buffer.getvalue().encode("utf-8"))


LOGS_HTML = """<!doctype html>
<html><head><meta charset="utf-8"><title>gengin request logs</title>
<style>
 body{background:#151517;color:#e9e9ec;font:13px/1.45 system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;margin:0;padding:18px 22px}
 h1{font-size:15px;font-weight:600;margin:0 0 12px;color:#cfcfd6}
 select,button{background:#232328;color:#e9e9ec;border:1px solid #3a3a42;border-radius:6px;padding:4px 8px;font:inherit}
 .cards{display:flex;flex-wrap:wrap;gap:10px;margin:12px 0 10px}
 .card{background:#1c1c21;border:1px solid #2b2b33;border-radius:8px;padding:8px 12px;min-width:120px}
 .card span{color:#8b8b94;font-size:11px;text-transform:uppercase;letter-spacing:.04em}
 .card b{display:block;font-size:16px;margin-top:2px;font-weight:600}
 .chart{display:flex;align-items:flex-end;gap:3px;height:140px;background:#1c1c21;border:1px solid #2b2b33;border-radius:8px;padding:22px 10px 10px;overflow:hidden}
 .bar{flex:1;background:#3ecf8e;min-width:3px;border-radius:2px 2px 0 0;position:relative}
 .bar:hover{background:#63e2aa}
 .barlabel{position:absolute;bottom:calc(100% + 4px);left:50%;transform:translateX(-50%);white-space:nowrap;font-size:10px;color:#a8a8b0;display:none;z-index:2}
 .bar:hover .barlabel{display:block}
 .empty{color:#77777f;padding:40px 10px;margin:auto}
 table{width:100%;border-collapse:collapse;margin-top:14px;font-size:12px}
 th,td{padding:4px 8px;text-align:left;border-bottom:1px solid #26262c;white-space:nowrap}
 th{color:#8b8b94;font-weight:500;position:sticky;top:0;background:#151517}
 tr:hover td{background:#1b1b20}
 td.num,th.num{text-align:right}
 .up{color:#8fb4ff}.ds{color:#3ecf8e}
 .muted{color:#77777f}
</style></head><body>
<h1>gengin request logs <span class="muted" id="range"></span></h1>
<div>
 window <select id="window">
  <option value="3600">1h</option><option value="86400" selected>24h</option>
  <option value="604800">7d</option><option value="2592000">30d</option><option value="0">all</option>
 </select>
 interval <select id="interval">
  <option value="auto" selected>auto</option>
  <option value="60">1m</option><option value="300">5m</option>
  <option value="900">15m</option><option value="3600">1h</option>
  <option value="21600">6h</option><option value="86400">1d</option>
 </select>
 metric <select id="metric">
  <option value="cost" selected>cost</option>
  <option value="req">requests</option>
  <option value="hit">cache-hit tokens</option>
  <option value="hit_pct">cache-hit %</option>
  <option value="input">input tokens</option>
  <option value="cost_session">cost / session</option>
 </select>
 <button onclick="load()">refresh</button>
 <button onclick="doExport('csv')">export CSV</button>
 <button onclick="doExport('json')">export JSON</button>
 <span class="muted">auto-refresh 30s</span>
</div>
<div class="cards" id="cards"></div>
<div class="chart" id="chart"></div>
<div style="overflow:auto;max-height:58vh"><table id="table"></table></div>
<script>
const $ = (id) => document.getElementById(id);
const fmtTok = (v) => v == null ? '-' : v >= 1e6 ? (v / 1e6).toFixed(2) + 'M' : v >= 1e3 ? (v / 1e3).toFixed(1) + 'k' : String(v);
const fmtCost = (v) => v == null ? '-' : '$' + Number(v).toFixed(5);
function card(label, value) { return '<div class="card"><span>' + label + '</span><b>' + value + '</b></div>'; }
function esc(text) { return String(text == null ? '' : text).replace(/[&<>"]/g, (c) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c])); }
function sessionName(r) { return (r.label || 'manual') + ' #' + (r.route_id || 'anon').slice(0, 6); }
const fmtPct = (hit, miss) => { const d = (hit || 0) + (miss || 0); return d > 0 ? (100 * (hit || 0) / d).toFixed(1) + '%' : '-'; };
const pad = (n) => String(n).padStart(2, '0');
const autoBucket = (w) => !w ? 86400 : w <= 7200 ? 60 : w <= 86400 ? 900 : w <= 259200 ? 3600 : w <= 7776000 ? 21600 : 86400;
const bucketName = (b) => b >= 86400 ? '1d' : b >= 21600 ? '6h' : b >= 3600 ? '1h' : b >= 900 ? '15m' : b >= 300 ? '5m' : '1m';
function fmtBucket(ts, bucket) { const d = new Date(ts * 1000); if (bucket >= 86400) return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()); return pad(d.getMonth() + 1) + '-' + pad(d.getDate()) + ' ' + pad(d.getHours()) + ':' + pad(d.getMinutes()); }
function doExport(format) { const w = Number($('window').value); location.href = '/logs/export?format=' + format + '&window=' + w; }
async function load() {
  const windowSec = Number($('window').value);
  const metric = $('metric').value;
  const bucket = $('interval').value === 'auto' ? autoBucket(windowSec) : Number($('interval').value);
  const res = await fetch('/logs/data?window=' + windowSec + '&limit=5000');
  const data = await res.json();
  const rows = data.rows || [];
  const totals = data.totals || {};
  $('range').textContent = (windowSec ? '(' + rows.length + ' rows shown)' : '(all time)') + ' - ' + bucketName(bucket) + ' buckets';
  $('cards').innerHTML = card('requests', totals.requests || 0) + card('cost', fmtCost(totals.cost)) + card('input', fmtTok(totals.prompt)) + card('output', fmtTok(totals.completion)) + card('cache-hit', fmtTok(totals.cache_hit)) + card('cache-hit %', fmtPct(totals.cache_hit, totals.cache_miss));
  const acc = new Map();
  const slot = (key) => { let a = acc.get(key); if (!a) { a = { v: 0, hit: 0, miss: 0 }; acc.set(key, a); } return a; };
  for (const r of rows) {
    const key = metric === 'cost_session' ? sessionName(r) : Math.floor(Number(r.ts) / bucket) * bucket;
    const a = slot(key);
    a.hit += r.cache_hit || 0;
    a.miss += r.cache_miss || 0;
    if (metric === 'cost' || metric === 'cost_session') a.v += r.cost || 0;
    else if (metric === 'req') a.v += 1;
    else if (metric === 'hit') a.v += r.cache_hit || 0;
    else if (metric === 'input') a.v += r.prompt || 0;
  }
  const value = (a) => metric === 'hit_pct' ? ((a.hit + a.miss) > 0 ? 100 * a.hit / (a.hit + a.miss) : 0) : a.v;
  let items = [...acc.entries()];
  if (metric === 'cost_session') { items.sort((a, b) => value(b[1]) - value(a[1])); items = items.slice(0, 24); }
  else { items.sort((a, b) => a[0] - b[0]); }
  const max = Math.max(...items.map((pair) => value(pair[1])), 0.000001);
  $('chart').innerHTML = items.length ? items.map((pair) => {
    const label = metric === 'cost_session' ? pair[0] : fmtBucket(pair[0], bucket);
    const v = value(pair[1]);
    const shown = metric === 'hit_pct' ? v.toFixed(1) + '%' : metric.includes('cost') ? fmtCost(v) : fmtTok(Math.round(v));
    return '<div class="bar" style="height:' + Math.max(2, (v / max) * 100) + '%"><span class="barlabel">' + esc(label) + ' - ' + shown + '</span></div>';
  }).join('') : '<div class="empty">no requests recorded yet</div>';
  const cols = [['date', 0], ['session', 0], ['model', 0], ['provider', 0], ['input', 1], ['output', 1], ['cached', 1], ['hit %', 1], ['cost', 1], ['ms', 1], ['status', 1]];
  let html = '<thead><tr>' + cols.map((c) => '<th' + (c[1] ? ' class="num"' : '') + '>' + c[0] + '</th>').join('') + '</tr></thead><tbody>';
  for (const r of rows.slice(0, 800)) {
    const provider = r.upstream === 'deepseek' ? '<span class="ds">deepseek</span>' : '<span class="up">openrouter</span>';
    html += '<tr><td>' + esc(r.date) + '</td><td>' + esc(sessionName(r)) + '</td><td>' + esc(r.model) + '</td><td>' + provider + '</td><td class="num">' + fmtTok(r.prompt) + '</td><td class="num">' + fmtTok(r.completion) + '</td><td class="num">' + fmtTok(r.cache_hit) + '</td><td class="num">' + fmtPct(r.cache_hit, r.cache_miss) + '</td><td class="num">' + fmtCost(r.cost) + '</td><td class="num">' + (r.ms == null ? '-' : r.ms) + '</td><td class="num">' + (r.status == null ? '-' : r.status) + '</td></tr>';
  }
  $('table').innerHTML = html + '</tbody>';
}
$('window').addEventListener('change', load);
$('metric').addEventListener('change', load);
$('interval').addEventListener('change', load);
load();
setInterval(load, 30000);
</script></body></html>
"""


class ProxyHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "gengin-openrouter-proxy"

    # -- helpers -----------------------------------------------------------

    def log_message(self, fmt, *args):  # silence default stderr noise
        return

    def _read_body(self):
        length = self.headers.get("Content-Length")
        if length is None:
            return b""
        try:
            n = int(length)
        except ValueError:
            return b""
        if n <= 0 or n > MAX_BODY:
            return b""
        return self.rfile.read(n)

    def _forward_headers(self):
        headers = {}
        for key, value in self.headers.items():
            if key.lower() in ("host", "content-length", "accept-encoding"):
                continue
            headers[key] = value
        # Keep responses plain so chunked relay is byte-exact.
        headers["Accept-Encoding"] = "identity"
        return headers

    def _send_upstream_status(self, status, headers):
        self.send_response_only(status)
        for key, value in headers.items():
            if key.lower() in (
                "connection", "keep-alive", "proxy-authenticate",
                "proxy-authorization", "te", "trailers", "transfer-encoding",
                "content-length", "upgrade",
            ):
                continue
            self.send_header(key, value)
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

    def _write_chunk(self, chunk):
        self.wfile.write(b"%X\r\n" % len(chunk))
        self.wfile.write(chunk)
        self.wfile.write(b"\r\n")

    def _relay_body(self, resp, buffered=None, tap=None):
        try:
            if buffered is not None:
                if buffered:
                    self._write_chunk(buffered)
                    if tap is not None:
                        tap.feed(buffered)
                self.wfile.write(b"0\r\n\r\n")
                return True
            reader = getattr(resp, "read1", None) or resp.read
            while True:
                chunk = reader(65536)
                if not chunk:
                    break
                self._write_chunk(chunk)
                if tap is not None:
                    tap.feed(chunk)
            self.wfile.write(b"0\r\n\r\n")
            return True
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            self.close_connection = True
            return False

    def _send_json(self, status, payload):
        data = json.dumps(payload, indent=2).encode() + b"\n"
        self.send_response_only(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _send_page(self, status, html):
        data = html.encode("utf-8")
        self.send_response_only(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _send_download(self, filename, content_type, body):
        self.send_response_only(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Disposition",
                         'attachment; filename="%s"' % filename)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _upstream_request(self, body, headers, upstream="openrouter", key=""):
        if upstream == "deepseek":
            headers = dict(headers)
            headers["Authorization"] = "Bearer " + key
            url = DEEPSEEK_UPSTREAM + _upstream_path(self.path, upstream)
        else:
            url = UPSTREAM + self.path
        req = urllib.request.Request(
            url, data=body if body is not None else None, method=self.command
        )
        for header, value in headers.items():
            req.add_header(header, value)
        return urllib.request.urlopen(req, timeout=TIMEOUT)

    # -- request routing ---------------------------------------------------

    def do_GET(self):
        if self.path == "/health":
            self._send_json(200, {"ok": True, "upstream": UPSTREAM, "port": PORT})
            return
        if self.path == "/route":
            self._send_json(200, _route_status())
            return
        if self.path == "/logs":
            self._send_page(200, LOGS_HTML)
            return
        if self.path.startswith("/logs/data"):
            query = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            self._send_json(200, _logs_data(query))
            return
        if self.path.startswith("/logs/export"):
            query = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            filename, content_type, body = _logs_export(query)
            self._send_download(filename, content_type, body)
            return
        if self.path == "/status":
            with _lock:
                models = {
                    model: dict(state) for model, state in _model_state.items()
                }
                stats = dict(_stats)
            self._send_json(200, {
                "config": {
                    "upstream": UPSTREAM,
                    "quantizations": QUANTIZATIONS,
                    "floor": ENABLE_FLOOR,
                    "allow_fallbacks": ALLOW_FALLBACKS,
                    "pin_fallbacks": PIN_FALLBACKS,
                    "fail_threshold": FAIL_THRESHOLD,
                    "cooldown_seconds": COOLDOWN_SECONDS,
                    "coach": {
                        "enabled": COACH_ENABLED,
                        "max_per_session": COACH_MAX,
                        "cooldown_seconds": COACH_COOLDOWN,
                        "hints": len(_load_hints()),
                    },
                },
                "stats": stats,
                "models": models,
                "route": _route_status(),
            })
            return
        self._passthrough()

    def do_POST(self):
        if self.command == "POST" and (
                self.path == "/route" or self.path.startswith("/route/")):
            self._handle_route()
            return
        with _lock:
            _stats["requests"] += 1
        raw = self._read_body()
        headers = self._forward_headers()

        try:
            body = json.loads(raw) if raw else {}
        except (json.JSONDecodeError, UnicodeDecodeError):
            self._passthrough(raw=raw, headers=headers)
            return

        model = body.get("model") if isinstance(body, dict) else None
        if not isinstance(model, str) or not model:
            self._passthrough(raw=raw, headers=headers)
            return

        # Context-aware route: pick the chain member before any filtering, so
        # the injected constraints and the fail-open retry apply to the leg
        # that actually serves this request.
        route_ready = _route_active() is not None
        conversation = _conversation_key(body) if (COACH_ENABLED or route_ready) else ""
        route_info = None
        if route_ready:
            model, route_info = _route_apply(body, model, len(raw), conversation)

        upstream_kind = route_info["upstream"] if route_info else "openrouter"
        upstream_key = ""
        if upstream_kind != "openrouter":
            upstream_key = (route_info["route"].get("deepseekKey")
                            or os.environ.get("DEEPSEEK_API_KEY", ""))
            if not upstream_key:
                _log_line(f"POST {self.path} model={model} error=no-route-key")
                self._send_json(502, {"error": {"message":
                    "proxy: the deepseek chain leg has no API key"}})
                return

        if route_info is not None:
            blocked = _budget_block(route_info["route"])
            if blocked:
                _log_line(f"POST {self.path} model={model} budget=block "
                          f"spent={route_info['route'].get('spentUsd', 0.0):.4f}")
                self._send_json(402, {"error": {"message": blocked}})
                return

        reason_field = ""
        cached = backfilled = 0
        if upstream_kind == "deepseek":
            # Thinking mode requires reasoning_content on the replayed
            # assistant messages; harnesses drop it, so restore what this
            # proxy served.  Runs before the fallback copy so a retry keeps
            # the field.
            cached, backfilled = _reasoning_replay(conversation, body)
            if cached:
                reason_field += f" reasoning={cached}"
            if backfilled:
                reason_field += f" backfill={backfilled}"

        original = copy.deepcopy(body)
        applied, skip_reason = _apply_filters(body, model, upstream_kind)

        # Coach note goes into this request only - never into `original`, so the
        # unfiltered retry still carries the caller's own turn.
        coach_note = ""
        budget_note = ""
        messages = body.get("messages")
        if isinstance(messages, list):
            coach_note = _coach_take(body)
            if coach_note:
                messages.append({"role": "user", "content": coach_note})
            if route_info is not None:
                budget_note = _budget_take(route_info["route"])
                if budget_note:
                    messages.append({"role": "user", "content": budget_note})

        started = time.monotonic()

        try:
            upstream = self._upstream_request(json.dumps(body).encode(), headers,
                                              upstream_kind, upstream_key)
        except urllib.error.HTTPError as exc:
            upstream = exc
        except OSError as exc:
            _log_line(
                f"POST {self.path} model={model} skip={skip_reason or '-'} "
                f"error={type(exc).__name__}"
            )
            self._send_json(502, {"error": {"message": f"proxy: upstream error: {exc}"}})
            return

        status = _status_of(upstream)
        retried = False
        buffered = None

        # Fail-open retry: the injected constraints can exclude every endpoint
        # of a model (e.g. closed models with no quantization metadata). Drop
        # them once; the error body is buffered so it can still be relayed.
        if applied and status in (400, 404):
            try:
                buffered = upstream.read()
            except OSError:
                buffered = b""
            text = buffered.decode("utf-8", "ignore").lower()
            retryable = status == 404 or any(
                token in text
                for token in ("provider", "endpoint", "quantization", "no allowed")
            )
            if retryable:
                with _lock:
                    _stats["retries"] += 1
                _record_result(model, ok=False)
                try:
                    upstream.close()
                except OSError:
                    pass
                try:
                    upstream = self._upstream_request(
                        json.dumps(original).encode(), headers,
                        upstream_kind, upstream_key
                    )
                except urllib.error.HTTPError as exc:
                    upstream = exc
                except OSError as exc:
                    _log_line(
                        f"POST {self.path} model={model} retry error={type(exc).__name__}"
                    )
                    self._send_json(502, {"error": {"message": f"proxy: upstream error: {exc}"}})
                    return
                retried = True
                buffered = None
                status = _status_of(upstream)

        if applied and 200 <= status < 300:
            _record_result(model, ok=True)

        streamed = "event-stream" in (upstream.headers.get("Content-Type") or "")
        elapsed_ms = int((time.monotonic() - started) * 1000)
        if applied or skip_reason:
            with _lock:
                _stats["injected" if applied else "skipped"] += 1
        route_field = ""
        if route_info is not None:
            route_field = (f" route={route_info['member']}"
                           f" est={route_info['est']}/{route_info['limit']}")
            if route_info["switched"]:
                route_field += " switched=1"
        _log_line(
            f"POST {self.path} model={model} inject={'+'.join(applied) or '-'} "
            f"skip={skip_reason or '-'} coach={1 if coach_note else 0} "
            f"budget={1 if budget_note else 0} "
            f"status={status} stream={1 if streamed else 0} "
            f"retry={1 if retried else 0} ms={elapsed_ms}{route_field}{reason_field}"
        )

        tap = (_ResponseTap(streamed)
               if (LOG_DB or conversation or route_info is not None) else None)
        try:
            self._send_upstream_status(_status_of(upstream), upstream.headers)
            self._relay_body(upstream, buffered, tap)
        finally:
            upstream.close()
        if tap is not None:
            _coach_observe(conversation, tap)
            _route_observe(conversation, route_info, tap, len(raw))
            _reasoning_observe(conversation, route_info, tap)
            entry = _log_entry(model, upstream_kind, route_info, conversation,
                               status, elapsed_ms, streamed, retried, applied,
                               skip_reason, bool(coach_note), bool(budget_note),
                               cached, backfilled, tap)
            if entry is not None:
                _log_request(entry)

    do_PUT = do_POST
    do_PATCH = do_POST

    def _handle_route(self):
        """Loopback registration endpoints for the context-aware route."""
        if self.client_address[0] not in ("127.0.0.1", "::1"):
            self._send_json(403, {"error": {"message": "route: loopback only"}})
            return
        raw = self._read_body()
        try:
            payload = json.loads(raw) if raw else {}
        except (json.JSONDecodeError, UnicodeDecodeError):
            self._send_json(400, {"error": {"message": "route: invalid JSON"}})
            return
        if self.path == "/route/register":
            status, response = _route_register(payload)
        elif self.path == "/route/clear":
            status, response = _route_clear(payload)
        else:
            status, response = 404, {"error": {"message": "route: unknown endpoint"}}
        self._send_json(status, response)

    def _passthrough(self, raw=None, headers=None):
        if raw is None:
            raw = self._read_body()
        if headers is None:
            headers = self._forward_headers()
        try:
            upstream = self._upstream_request(raw if raw else None, headers)
        except urllib.error.HTTPError as exc:
            upstream = exc
        except OSError as exc:
            self._send_json(502, {"error": {"message": f"proxy: upstream error: {exc}"}})
            return
        try:
            self._send_upstream_status(_status_of(upstream), upstream.headers)
            self._relay_body(upstream)
        finally:
            upstream.close()


def main():
    server = ThreadingHTTPServer((BIND, PORT), ProxyHandler)
    server.daemon_threads = True
    _log_line(f"listening on http://{BIND}:{PORT} -> {UPSTREAM} (port {PORT})")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
