#!/usr/bin/env python3
"""DeepSeek direct-API pricing: per-response cost + the off-peak calendar.

Pure stdlib; used by the proxy (cost accounting, session budget, request log)
and by the launchers (peak-hour warnings / waiting).  Prices are per 1M tokens
from https://api-docs.deepseek.com/quick_start/pricing/ (peak / off-peak):

    model              cache-hit          cache-miss         output
    deepseek-flash     $0.006 / $0.003    $0.30 / $0.15      $1.20 / $0.60
    deepseek-v4-pro    $0.044 / $0.022    $1.32 / $0.66      $3.96 / $1.98

Off-peak rates are half of the peak rates.  Peak hours are 01:00-04:00 and
06:00-10:00 UTC, Monday-Friday; every other hour is off-peak, including whole
weekends.  Chinese public holidays are off-peak in full too - that part is not
modeled here (the calendar is not available offline), so the peak windows are
treated as peak even on those days.

CLI:
    deepseek_pricing.py status   # human line; exit 0 = off-peak, 2 = peak
    deepseek_pricing.py wait     # sleep until off-peak, then exit 0
    deepseek_pricing.py cost MODEL PROMPT COMPLETION HIT MISS   # USD for one call
"""

import sys
import time

PEAK_WINDOWS_UTC = ((1 * 60, 4 * 60), (6 * 60, 10 * 60))  # minutes since midnight

FLASH = {"hit": 0.006, "miss": 0.30, "out": 1.20}
PRO = {"hit": 0.044, "miss": 1.32, "out": 3.96}
PRICING = {
    "deepseek-flash": FLASH,
    "deepseek-v4-flash": FLASH,   # legacy alias, billed at Flash price
    "deepseek-v4-flash-vision-exp": FLASH,
    "deepseek-v4-pro": PRO,
    "deepseek-pro": PRO,
}


def prices_for(model):
    """Price table for a model id, or None when it is not a known DeepSeek model."""
    name = (model or "").strip().lower()
    if name in PRICING:
        return PRICING[name]
    if "deepseek" not in name:
        return None
    return PRO if "pro" in name else FLASH


def is_offpeak(when=None):
    """True when `when` (epoch seconds, default now) is outside the peak windows."""
    utc = time.gmtime(when if when is not None else time.time())
    if utc.tm_wday >= 5:  # Saturday / Sunday
        return True
    minutes = utc.tm_hour * 60 + utc.tm_min
    return not any(start <= minutes < end for start, end in PEAK_WINDOWS_UTC)


def next_change(when=None):
    """Epoch seconds of the next peak <-> off-peak transition."""
    start = when if when is not None else time.time()
    state = is_offpeak(start)
    minute = int(start // 60) * 60 + 60
    for _ in range(8 * 24 * 60 + 2):
        if is_offpeak(minute) != state:
            return minute
        minute += 60
    return start + 86400


def cost_usd(model, usage, when=None):
    """Cost of one response from its `usage` dict, or None when not computable.

    Reads DeepSeek's own cache split (`prompt_cache_hit_tokens` /
    `prompt_cache_miss_tokens`) and falls back to the nested
    `prompt_tokens_details.cached_tokens` used by OpenRouter-style usage.
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
    factor = 0.5 if is_offpeak(when) else 1.0
    return factor * (hit * prices["hit"] + miss * prices["miss"]
                     + completion * prices["out"]) / 1e6


def main(argv):
    command = argv[1] if len(argv) > 1 else "status"
    if command == "status":
        off = is_offpeak()
        label = "OFF-PEAK (50% off)" if off else "PEAK (full price)"
        change = time.strftime("%a %H:%M UTC", time.gmtime(next_change()))
        print(f"deepseek: {label}; next change {change}")
        return 0 if off else 2
    if command == "wait":
        while not is_offpeak():
            change = next_change()
            wait = max(1.0, change - time.time() + 1)
            print(f"waiting for off-peak: {int(wait // 60)} min "
                  f"(until {time.strftime('%a %H:%M UTC', time.gmtime(change))})",
                  flush=True)
            time.sleep(min(wait, 3600.0))
        print("off-peak window reached", flush=True)
        return 0
    if command == "cost" and len(argv) == 7:
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
