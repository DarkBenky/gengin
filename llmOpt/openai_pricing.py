#!/usr/bin/env python3
"""OpenAI direct-API model catalog and pricing.

Model ids and prices are not hardcoded here: the catalog is fetched from
LiteLLM's public model_prices_and_context_window.json and cached locally.
Only entries with `litellm_provider == "openai"` are kept; costs are
normalized to USD per 1M tokens (in / cached / out) and paired with the
input context limit.

Source:
    https://raw.githubusercontent.com/BerriAI/litellm/main/model_prices_and_context_window.json

The proxy never blocks a request on the network: it reads the local cache,
and a background refresh runs at startup when the cache is stale.  Launcher
and supervisor paths call ensure_catalog() and may wait for the fetch.  A
failed fetch keeps the stale cache; with no cache, costs are reported as
None (tokens are still logged).

Environment:
    GENGIN_OPENAI_PRICING_URL      catalog source (default: LiteLLM JSON)
    GENGIN_OPENAI_PRICING_CACHE    cache file (default: llmOpt/state/openai_pricing.json)
    GENGIN_OPENAI_PRICING_TTL      refresh interval seconds (default 86400, 0 = never)
    GENGIN_OPENAI_PRICING_TIMEOUT  fetch timeout seconds (default 5)
    GENGIN_OPENAI_MODEL_PREFERENCE comma-separated substrings for default selection

CLI:
    openai_pricing.py cost MODEL PROMPT COMPLETION HIT MISS
    openai_pricing.py models [--all] [--json]
    openai_pricing.py default [--preference TEXT]
    openai_pricing.py refresh
    openai_pricing.py status
"""

import json
import os
import re
import sys
import tempfile
import threading
import time
import urllib.request

DEFAULT_URL = (
    "https://raw.githubusercontent.com/BerriAI/litellm/main/"
    "model_prices_and_context_window.json")
DEFAULT_TTL = 86400
DEFAULT_TIMEOUT = 5.0

_lock = threading.Lock()
_refresh_lock = threading.Lock()
_async = {"thread": None}
_memory = {"mtime": None, "models": {}}
_last_error = ""

_DATE_RE = re.compile(r"-\d{4}-\d{2}-\d{2}$")
_DATE8_RE = re.compile(r"-\d{8}$")
_VERSION_RE = re.compile(r"(\d+(?:\.\d+)*)")
# Specialised families are excluded from automatic selection; they stay in
# the catalog, so an explicit model id still resolves and prices correctly.
_SPECIAL_RE = re.compile(
    r"(^ft:|audio|search|computer-use|preview|realtime|transcri|tts|"
    r"embedding|moderation|image|dall-e|sora|deep-research|codex)")


def _cache_file():
    return os.environ.get("GENGIN_OPENAI_PRICING_CACHE") or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "state", "openai_pricing.json")


def _source_url():
    return (os.environ.get("GENGIN_OPENAI_PRICING_URL") or DEFAULT_URL).strip()


def _ttl():
    try:
        return int(float(os.environ.get("GENGIN_OPENAI_PRICING_TTL", DEFAULT_TTL)))
    except (TypeError, ValueError):
        return DEFAULT_TTL


def _timeout():
    try:
        return float(os.environ.get("GENGIN_OPENAI_PRICING_TIMEOUT", DEFAULT_TIMEOUT))
    except (TypeError, ValueError):
        return DEFAULT_TIMEOUT


def _normalize(model):
    name = (model or "").strip().lower()
    if name.startswith("openai/"):
        name = name[len("openai/"):]
    return name


def _is_dated(name):
    return bool(_DATE_RE.search(name) or _DATE8_RE.search(name))


def _short_name(name):
    name = _DATE_RE.sub("", name)
    name = _DATE8_RE.sub("", name)
    if name.endswith("-latest"):
        name = name[:-len("-latest")]
    return name


def _version_key(name):
    match = _VERSION_RE.search(_short_name(name))
    return tuple(int(part) for part in match.group(1).split(".")) if match else ()


def _is_special(name):
    return bool(_SPECIAL_RE.search(name))


def _parse_catalog(raw):
    """Compact LiteLLM's catalog: OpenAI chat entries, USD per 1M tokens."""
    models = {}
    today = time.strftime("%Y-%m-%d")
    for name, entry in raw.items():
        if not isinstance(entry, dict):
            continue
        if entry.get("litellm_provider") != "openai":
            continue
        mode = entry.get("mode")
        if mode not in ("chat", "responses"):
            continue
        in_cost = entry.get("input_cost_per_token")
        out_cost = entry.get("output_cost_per_token")
        if (not isinstance(in_cost, (int, float)) or isinstance(in_cost, bool)
                or not isinstance(out_cost, (int, float))
                or isinstance(out_cost, bool)):
            continue
        cached = entry.get("cache_read_input_token_cost")
        if not isinstance(cached, (int, float)) or isinstance(cached, bool):
            cached = in_cost
        deprecation = str(entry.get("deprecation_date") or "")
        models[str(name).lower()] = {
            "in": float(in_cost) * 1e6,
            "cached": float(cached) * 1e6,
            "out": float(out_cost) * 1e6,
            "context": int(entry.get("max_input_tokens")
                           or entry.get("max_tokens") or 0),
            "mode": mode,
            "deprecated": bool(deprecation and deprecation[:10] <= today),
        }
    return models


def _write_cache(models):
    path = _cache_file()
    directory = os.path.dirname(path) or "."
    os.makedirs(directory, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".openai_pricing.", dir=directory)
    try:
        with os.fdopen(fd, "w") as fh:
            json.dump({"source": _source_url(), "fetchedAt": time.time(),
                       "models": models}, fh)
        os.replace(tmp, path)
    except OSError:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def read_catalog():
    """Cached OpenAI model catalog (no network access)."""
    path = _cache_file()
    try:
        mtime = os.path.getmtime(path)
    except OSError:
        return {}
    with _lock:
        if _memory["mtime"] == mtime:
            return _memory["models"]
        try:
            with open(path) as fh:
                data = json.load(fh)
            models = data.get("models") if isinstance(data, dict) else {}
        except (OSError, ValueError):
            models = {}
        if not isinstance(models, dict):
            models = {}
        _memory["mtime"] = mtime
        _memory["models"] = models
        return models


def _stale():
    ttl = _ttl()
    if ttl <= 0:
        return False
    try:
        age = time.time() - os.path.getmtime(_cache_file())
    except OSError:
        return True
    return age > ttl


def ensure_catalog(force=False):
    """Return the catalog, refreshing from the source when missing or stale."""
    global _last_error
    cached = read_catalog()
    if cached and not force and not _stale():
        return cached
    with _refresh_lock:
        cached = read_catalog()
        if cached and not force and not _stale():
            return cached
        try:
            with urllib.request.urlopen(_source_url(), timeout=_timeout()) as resp:
                raw = json.load(resp)
            models = _parse_catalog(raw) if isinstance(raw, dict) else {}
            if not models:
                raise ValueError("no priced OpenAI models in the catalog")
        except Exception as exc:  # network, JSON, bad payload
            _last_error = f"{type(exc).__name__}: {exc}"
            return cached
        try:
            _write_cache(models)
            _last_error = ""
        except OSError as exc:
            # Keep pricing in this process even when the cache is read-only.
            _last_error = f"cache write failed: {exc}"
        return models


def refresh_async():
    """Kick off a background refresh when the cache is stale (proxy startup)."""
    if not _stale():
        return
    with _lock:
        thread = _async["thread"]
        if thread is not None and thread.is_alive():
            return
        thread = threading.Thread(target=ensure_catalog, daemon=True,
                                  name="openai-pricing-refresh")
        _async["thread"] = thread
        thread.start()


def reset_cache():
    """Clear the in-process cache (tests and long-lived reloads)."""
    global _last_error
    with _lock:
        _memory["mtime"] = None
        _memory["models"] = {}
        _last_error = ""


def prices_for(model):
    """Price table for a model id (USD per 1M), or None when unknown."""
    name = _normalize(model)
    models = read_catalog()
    if not name or not models:
        return None
    key = name if name in models else None
    if key is None:
        short = _short_name(name)
        if short in models:
            key = short
    if key is None:
        prefix = ""
        for candidate in models:
            if name.startswith(candidate + "-") and len(candidate) > len(prefix):
                prefix = candidate
        if prefix:
            key = prefix
    if key is None:
        family = _short_name(name)
        matches = [candidate for candidate in models
                   if _short_name(candidate) == family]
        if matches:
            undated = [candidate for candidate in matches
                       if not _is_dated(candidate)]
            key = min(undated) if undated else max(matches)
    if key is None:
        return None
    info = models[key]
    return {"in": info["in"], "cached": info["cached"], "out": info["out"]}


def _selectable(name, info):
    return (info.get("mode") == "chat" and not info.get("deprecated")
            and not name.startswith("ft:") and not _is_special(name))


def _best_model(names):
    return max(names, key=lambda name: (_version_key(name),
                                        not _is_dated(name), name))


def chat_models(include_all=False, include_deprecated=False):
    """Priced catalog rows; chat models only unless include_all is set."""
    rows = []
    for name, info in read_catalog().items():
        if not include_all:
            if (info.get("mode") != "chat" or _is_special(name)
                    or name.startswith("ft:")):
                continue
            if info.get("deprecated") and not include_deprecated:
                continue
        rows.append({"model": name, "in": info["in"], "cached": info["cached"],
                     "out": info["out"], "context": info.get("context", 0),
                     "mode": info.get("mode", ""),
                     "deprecated": bool(info.get("deprecated"))})
    return sorted(rows, key=lambda row: row["model"])


def pick_default(preferences=""):
    """Auto-select a chat model: first matching preference, then newest."""
    models = ensure_catalog()
    names = [name for name, info in models.items() if _selectable(name, info)]
    if not names:
        return ""
    tokens = [token.strip().lower()
              for token in (preferences or "").split(",") if token.strip()]
    for token in tokens:
        matches = [name for name in names if token in name]
        if matches:
            return _best_model(matches)
    return _best_model(names)


def pick_alias(model):
    """A different real model for the never-reached direct-session fallback."""
    models = ensure_catalog()
    name = _normalize(model)
    names = [candidate for candidate, info in models.items()
             if _selectable(candidate, info) and candidate != name]
    if names:
        return _best_model(names)
    return (name or "openai") + "-fallback"


def cost_usd(model, usage):
    """Cost of one response from its `usage` dict, or None when not computable.

    Reads the nested `prompt_tokens_details.cached_tokens` split used by the
    OpenAI API and falls back to the flat `prompt_cache_hit_tokens` /
    `prompt_cache_miss_tokens` fields used by OpenRouter-style usage.
    """
    prices = prices_for(model)
    if prices is None or not isinstance(usage, dict):
        return None
    prompt = usage.get("prompt_tokens")
    completion = usage.get("completion_tokens")
    if not isinstance(prompt, int) or isinstance(prompt, bool):
        return None

    def _int(value):
        return value if isinstance(value, int) and not isinstance(value, bool) else None

    hit = _int(usage.get("prompt_cache_hit_tokens"))
    miss = _int(usage.get("prompt_cache_miss_tokens"))
    if hit is None:
        details = usage.get("prompt_tokens_details")
        hit = _int(details.get("cached_tokens")) if isinstance(details, dict) else None
    hit = hit or 0
    miss = miss if miss is not None else max(0, prompt - hit)
    completion = completion if isinstance(completion, int) and not isinstance(completion, bool) else 0
    return (hit * prices["cached"] + miss * prices["in"]
            + completion * prices["out"]) / 1e6


def status():
    """Cache state for the CLI (path, age, source, model count)."""
    try:
        age = int(time.time() - os.path.getmtime(_cache_file()))
    except OSError:
        age = None
    return {"cache": _cache_file(), "age": age, "ttl": _ttl(),
            "source": _source_url(), "models": len(read_catalog()),
            "error": _last_error}


def main(argv):
    command = argv[1] if len(argv) > 1 else ""
    if command == "cost" and len(argv) == 7:
        try:
            usage = {"prompt_tokens": int(argv[3]),
                     "completion_tokens": int(argv[4]),
                     "prompt_cache_hit_tokens": int(argv[5]),
                     "prompt_cache_miss_tokens": int(argv[6])}
        except ValueError:
            print("cost expects integer token counts", file=sys.stderr)
            return 2
        value = cost_usd(argv[2], usage)
        print(f"${value:.6f}" if value is not None else "unknown model")
        return 0
    if command == "models":
        rows = chat_models(include_all="--all" in argv)
        if "--json" in argv:
            print(json.dumps(rows, indent=2))
            return 0 if rows else 1
        if not rows:
            print("(no catalog: run `openai_pricing.py refresh`)", file=sys.stderr)
            return 1
        print(f"{'model':44} {'in $/M':>9} {'cache $/M':>10} "
              f"{'out $/M':>9} {'ctx':>9}")
        for row in rows:
            print(f"{row['model']:44} {row['in']:9.3f} {row['cached']:10.3f} "
                  f"{row['out']:9.3f} {row['context']:9d}")
        return 0
    if command == "default":
        preference = os.environ.get("GENGIN_OPENAI_MODEL_PREFERENCE", "")
        if "--preference" in argv:
            index = argv.index("--preference")
            if index + 1 < len(argv):
                preference = argv[index + 1]
        model = pick_default(preference)
        if not model:
            print("no model resolved (run `openai_pricing.py refresh`)",
                  file=sys.stderr)
            return 1
        print(model)
        return 0
    if command == "refresh":
        models = ensure_catalog(force=True)
        if models and not _last_error:
            print(f"refreshed {len(models)} OpenAI models from {_source_url()}")
            return 0
        detail = _last_error or "no models"
        if models:
            print(f"refresh failed, keeping {len(models)} cached models: {detail}",
                  file=sys.stderr)
        else:
            print(f"refresh failed: {detail}", file=sys.stderr)
        return 1
    if command == "status":
        info = status()
        age = "never" if info["age"] is None else f"{info['age']}s"
        print(f"cache:  {info['cache']}")
        print(f"age:    {age} (ttl {info['ttl']}s)")
        print(f"source: {info['source']}")
        print(f"models: {info['models']}")
        if info["error"]:
            print(f"error:  {info['error']}")
        return 0
    print(__doc__.strip())
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
