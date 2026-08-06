"""
model_config.py -- Centralized model selection for the gengin optimization harness.

Three independent model roles, each configurable via env vars or API:
  MAIN_MODEL     — drives the optimization loop (default: deepseek-v4-flash-0731)
  REVIEW_MODEL   — skeptical pre-commit code review (default: deepseek-v4-flash-0731)
  RESEARCH_MODEL — read-only codebase exploration sub-agent (default: deepseek-v4-flash-0731)

Environment variable overrides (set before import):
  GENGIN_MAIN_MODEL       GENGIN_MAIN_PROVIDER
  GENGIN_REVIEW_MODEL     GENGIN_REVIEW_PROVIDER
  GENGIN_RESEARCH_MODEL   GENGIN_RESEARCH_PROVIDER
  GENGIN_BACKEND          openrouter | deepseek   (default: openrouter)
  GENGIN_COST_FIRST       1 | 0                  (default: 1 — cheapest provider via OpenRouter)

Usage:
    from model_config import FLASH, PRO, getConfig
    from model_config import setReviewModel, setResearchModel, setBackend

    setBackend("deepseek")          # switch to DeepSeek direct API
    cfg = getConfig()               # full three-role dict + backend/cost_first
"""

import os

# Canonical model identifiers
FLASH = "deepseek/deepseek-v4-flash-0731"
PRO   = "deepseek/deepseek-v4-pro"

# Provider — use DeepSeek's own infrastructure via OpenRouter routing.
# "deepseek" routes directly to DeepSeek API, avoiding third-party resellers.
PROVIDER_DEEPSEEK = "deepseek"

# Backend: "openrouter" (default) routes via OpenRouter with floor pricing.
# "deepseek" calls DeepSeek's own API directly (needs DEEPSEEK_API_KEY in .env).
BACKEND = os.environ.get("GENGIN_BACKEND", "openrouter")

# Cost-first: when True, OpenRouter selects the cheapest provider for every call.
# Equivalent to provider.sort:"price" in the OpenRouter API.
COST_FIRST = os.environ.get("GENGIN_COST_FIRST", "1") != "0"

# Three independent model slots with per-role defaults.
# MAIN_MODEL drives the primary optimization agent (planning, editing, PRs).
# REVIEW_MODEL is the skeptical second pair of eyes before commits.
# RESEARCH_MODEL is a cheaper model for read-only codebase exploration.
_models = {
    "main":     {"model": FLASH, "provider": PROVIDER_DEEPSEEK},
    "review":   {"model": FLASH, "provider": PROVIDER_DEEPSEEK},
    "research": {"model": FLASH, "provider": PROVIDER_DEEPSEEK},
}


def _envOverride(role_key: str):
    """Apply environment variable overrides for a given role, if set."""
    role_upper = role_key.upper()
    env_model = os.environ.get(f"GENGIN_{role_upper}_MODEL")
    env_prov  = os.environ.get(f"GENGIN_{role_upper}_PROVIDER")
    if env_model:
        _models[role_key]["model"] = env_model
    if env_prov:
        _models[role_key]["provider"] = env_prov


# Apply env overrides at import time so they take effect before any LLM call.
for _role in ("main", "review", "research"):
    _envOverride(_role)


# ---------------------------------------------------------------------------
# Per-role setters
# ---------------------------------------------------------------------------

def setModel(model_id: str) -> str:
    """Set the MAIN model (drives the optimization loop). Returns confirmation."""
    global _models
    old = _models["main"]["model"]
    _models["main"]["model"] = model_id
    _applyToMain()
    return f"Main model: {old} -> {model_id} (provider: {_models['main']['provider']})."


def setReviewModel(model_id: str) -> str:
    """Set the REVIEW model (skeptical pre-commit review). Returns confirmation."""
    old = _models["review"]["model"]
    _models["review"]["model"] = model_id
    _applyToMain()
    return f"Review model: {old} -> {model_id} (provider: {_models['review']['provider']})."


def setResearchModel(model_id: str) -> str:
    """Set the RESEARCH model (read-only sub-agent). Returns confirmation."""
    old = _models["research"]["model"]
    _models["research"]["model"] = model_id
    _applyToMain()
    return f"Research model: {old} -> {model_id} (provider: {_models['research']['provider']})."


# ---------------------------------------------------------------------------
# Per-role provider setters
# ---------------------------------------------------------------------------

def setProvider(provider: str) -> str:
    """Set the MAIN provider. Returns confirmation."""
    old = _models["main"]["provider"]
    _models["main"]["provider"] = provider
    _applyToMain()
    return f"Main provider: {old!r} -> {provider!r}."


def setReviewProvider(provider: str) -> str:
    """Set the REVIEW provider. Returns confirmation."""
    old = _models["review"]["provider"]
    _models["review"]["provider"] = provider
    _applyToMain()
    return f"Review provider: {old!r} -> {provider!r}."


def setResearchProvider(provider: str) -> str:
    """Set the RESEARCH provider. Returns confirmation."""
    old = _models["research"]["provider"]
    _models["research"]["provider"] = provider
    _applyToMain()
    return f"Research provider: {old!r} -> {provider!r}."


# ---------------------------------------------------------------------------
# Backend and cost-first control
# ---------------------------------------------------------------------------

def setBackend(backend: str) -> str:
    """Set the LLM backend: 'openrouter' (default) or 'deepseek' (direct API)."""
    global BACKEND
    if backend not in ("openrouter", "deepseek"):
        return f"Invalid backend {backend!r} — must be 'openrouter' or 'deepseek'."
    old = BACKEND
    BACKEND = backend
    _applyToMain()
    return f"Backend: {old} -> {backend}."


def getBackend() -> str:
    """Return the current LLM backend."""
    return BACKEND


def setCostFirst(enabled: bool) -> str:
    """Enable/disable cheapest-provider selection on OpenRouter."""
    global COST_FIRST
    old = COST_FIRST
    COST_FIRST = bool(enabled)
    _applyToMain()
    return f"Cost-first: {old} -> {COST_FIRST}."


def getCostFirst() -> bool:
    """Return whether cheapest-provider selection is enabled."""
    return COST_FIRST


# ---------------------------------------------------------------------------
# Getters
# ---------------------------------------------------------------------------

def getConfig() -> dict:
    """Return the full three-role model configuration as a dict."""
    return {
        "available_models": [FLASH, PRO],
        "backend": BACKEND,
        "cost_first": COST_FIRST,
        "roles": {
            role: {"model": cfg["model"], "provider": cfg["provider"]}
            for role, cfg in _models.items()
        },
    }


def getModel() -> str:
    """Return the MAIN model ID."""
    return _models["main"]["model"]


def getProvider() -> str:
    """Return the MAIN provider."""
    return _models["main"]["provider"]


def getReviewModel() -> str:
    """Return the REVIEW model ID."""
    return _models["review"]["model"]


def getReviewProvider() -> str:
    """Return the REVIEW provider."""
    return _models["review"]["provider"]


def getResearchModel() -> str:
    """Return the RESEARCH model ID."""
    return _models["research"]["model"]


def getResearchProvider() -> str:
    """Return the RESEARCH provider."""
    return _models["research"]["provider"]


# ---------------------------------------------------------------------------
# Bridge to main.py globals
# ---------------------------------------------------------------------------

def _applyToMain():
    """Push all three model selections into main.py's module-level globals.

    Sets REVIEWER_MODEL, REVIEWER_PROVIDER, RESEARCH_MODEL, RESEARCH_PROVIDER
    so all internal LLM calls (review, research, sync) use the correct model.

    Also pushes BACKEND and COST_FIRST into modelSelector globals so getResponse
    dispatches correctly.

    Called automatically by every setter.  Safe to call before main.py import.
    """
    try:
        import main as _main
        _main.REVIEWER_MODEL = _models["review"]["model"]
        _main.REVIEWER_PROVIDER = _models["review"]["provider"]
        _main.RESEARCH_MODEL = _models["research"]["model"]
        _main.RESEARCH_PROVIDER = _models["research"]["provider"]
        _main.SKEPTICAL_REVIEW_MODEL = _models["review"]["model"]
        _main.SKEPTICAL_REVIEW_PROVIDER = _models["review"]["provider"]
    except ImportError:
        pass  # main.py not yet imported — applied at MCP server startup

    try:
        import modelSelector as _ms
        _ms.COST_FIRST = COST_FIRST
        _ms.BACKEND = BACKEND
    except ImportError:
        pass
