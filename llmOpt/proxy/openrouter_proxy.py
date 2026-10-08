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
(routeId-guarded).

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
"""

import copy
import hashlib
import json
import os
import random
import re
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

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
                 "maxContext": spec["maxContext"], "expiresAt": spec["expiresAt"]}


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


def _route_observe(conversation, info, tap, raw_len):
    """Record the response's usage for the next request's estimate."""
    if info is None or tap is None:
        return
    usage = tap.usage
    if not isinstance(usage, dict):
        return
    route = info["route"]
    member = info["member"]
    prompt = usage.get("prompt_tokens")
    completion = usage.get("completion_tokens")
    cost = usage.get("cost")
    with _lock:
        counters = route["counters"][member]
        counters["requests"] += 1
        if isinstance(prompt, int) and not isinstance(prompt, bool):
            counters["promptTokens"] += prompt
        if isinstance(completion, int) and not isinstance(completion, bool):
            counters["completionTokens"] += completion
        if isinstance(cost, (int, float)) and not isinstance(cost, bool):
            counters["costUsd"] += float(cost)
        if (conversation and isinstance(prompt, int)
                and not isinstance(prompt, bool) and prompt > 0):
            state = route["conversations"].get(conversation)
            if state is None:
                state = {"member": member, "lastPrompt": 0, "lastChars": 0}
                route["conversations"][conversation] = state
            state["lastPrompt"] = prompt
            state["lastChars"] = raw_len


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

        reason_field = ""
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
        messages = body.get("messages")
        if isinstance(messages, list):
            coach_note = _coach_take(body)
            if coach_note:
                messages.append({"role": "user", "content": coach_note})

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
            f"status={status} stream={1 if streamed else 0} "
            f"retry={1 if retried else 0} ms={elapsed_ms}{route_field}{reason_field}"
        )

        tap = (_ResponseTap(streamed)
               if (conversation or route_info is not None) else None)
        try:
            self._send_upstream_status(_status_of(upstream), upstream.headers)
            self._relay_body(upstream, buffered, tap)
        finally:
            upstream.close()
        if tap is not None:
            _coach_observe(conversation, tap)
            _route_observe(conversation, route_info, tap, len(raw))
            _reasoning_observe(conversation, route_info, tap)

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
