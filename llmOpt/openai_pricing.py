#!/usr/bin/env python3
"""OpenAI direct-API pricing: per-response cost in USD.

Pure stdlib; used by the proxy (cost accounting, session budget, request
log) on the direct OpenAI leg.  Prices are per 1M tokens,
in / cached / out:

    model              in        cached    out
    gpt-6-astra        $10       $1        $50
    gpt-6.1-sol        $2        $0.10     $10
    gpt-6-sol          $2        $0.20     $10
    gpt-6-luna         $0.10     $0.01     $0.50
    gpt-5.6-sol        $4        $0.40     $20
    gpt-5.6-terra      $2        $0.20     $12
    gpt-5.6-luna       $0.20     $0.02     $1.20
    gpt-5.3            $1.75     $0.175    $14
    gpt-5.2            $1.75     $0.175    $14

Dated snapshot ids (e.g. gpt-5.3-2026-07-14) fall back to their family
price via substring hints.  Unknown ids cost None (the caller then logs
the response without a cost).  Unlike DeepSeek there is no off-peak
calendar: OpenAI prices are flat around the clock, so there is no
status/wait command here.

CLI:
    openai_pricing.py cost MODEL PROMPT COMPLETION HIT MISS   # USD for one call
"""

import sys

PRICING = {
    "gpt-6-astra": {"in": 10.0, "cached": 1.0, "out": 50.0},
    "gpt-6.1-sol": {"in": 2.0, "cached": 0.10, "out": 10.0},
    "gpt-6-sol": {"in": 2.0, "cached": 0.20, "out": 10.0},
    "gpt-6-luna": {"in": 0.10, "cached": 0.01, "out": 0.50},
    "gpt-5.6-sol": {"in": 4.0, "cached": 0.40, "out": 20.0},
    "gpt-5.6-terra": {"in": 2.0, "cached": 0.20, "out": 12.0},
    "gpt-5.6-luna": {"in": 0.20, "cached": 0.02, "out": 1.20},
    "gpt-5.3": {"in": 1.75, "cached": 0.175, "out": 14.0},
    "gpt-5.2": {"in": 1.75, "cached": 0.175, "out": 14.0},
}

# Substring hints for dated snapshot ids, most specific family first: the
# dated 5.6 families must win over the plain "sol"/"luna" hints, so they
# come first in the tuple.
FAMILY_HINTS = (
    ("astra", "gpt-6-astra"),
    ("6.1-sol", "gpt-6.1-sol"),
    ("5.6-sol", "gpt-5.6-sol"),
    ("5.6-terra", "gpt-5.6-terra"),
    ("5.6-luna", "gpt-5.6-luna"),
    ("sol", "gpt-6-sol"),
    ("luna", "gpt-6-luna"),
    ("5.3", "gpt-5.3"),
    ("5.2", "gpt-5.2"),
)


def prices_for(model):
    """Price table for a model id, or None when it is not a known GPT model."""
    name = (model or "").strip().lower()
    if name in PRICING:
        return PRICING[name]
    if "gpt" not in name:
        return None
    for hint, family in FAMILY_HINTS:
        if hint in name:
            return PRICING[family]
    return None


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


def main(argv):
    if len(argv) == 7 and argv[1] == "cost":
        usage = {"prompt_tokens": int(argv[3]), "completion_tokens": int(argv[4]),
                 "prompt_cache_hit_tokens": int(argv[5]),
                 "prompt_cache_miss_tokens": int(argv[6])}
        value = cost_usd(argv[2], usage)
        print(f"${value:.6f}" if value is not None else "unknown model")
        return 0
    print(__doc__.strip())
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
