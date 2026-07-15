"""
model_config.py -- Centralized model selection for the gengin optimization harness.

Provides easy switching between DeepSeek models and providers without modifying
main.py.  Used by both the MCP server (mcp_server.py) and the standalone loop
(main.py).

Usage:
    from model_config import setModel, FLASH, PRO, getConfig

    setModel(PRO)           # use deepseek-v4-pro (default)
    setModel(FLASH)         # use deepseek-v4-flash (cheaper/faster)
    cfg = getConfig()       # -> {"review": (model, provider), "research": ...}
"""

# Canonical model identifiers
FLASH = "deepseek/deepseek-v4-flash"
PRO   = "deepseek/deepseek-v4-pro"

# Provider — use DeepSeek's own infrastructure via OpenRouter routing.
# "deepseek" routes directly to DeepSeek API, avoiding third-party resellers.
PROVIDER_DEEPSEEK = "deepseek"

# Current active model
_current_model = PRO
_current_provider = PROVIDER_DEEPSEEK


def setModel(model_id: str) -> str:
    """Switch the active model.  Returns a confirmation string.

    Args:
        model_id: Either FLASH or PRO constant.
    """
    global _current_model
    if model_id not in (FLASH, PRO):
        return f"Unknown model: {model_id!r}. Use FLASH or PRO."
    old = _current_model
    _current_model = model_id
    _applyToMain()
    return f"Switched from {old} to {_current_model} (provider: {_current_provider})."


def setProvider(provider: str) -> str:
    """Switch the provider used for OpenRouter routing.  Returns confirmation."""
    global _current_provider
    old = _current_provider
    _current_provider = provider
    _applyToMain()
    return f"Switched provider from {old!r} to {_current_provider!r}."


def getConfig() -> dict:
    """Return the current model configuration as a dict."""
    return {
        "active_model": _current_model,
        "provider": _current_provider,
        "available_models": [FLASH, PRO],
        "model_for": {
            "review": (_current_model, _current_provider),
            "research": (_current_model, _current_provider),
            "sync": (_current_model, _current_provider),
        },
    }


def getModel() -> str:
    """Return the currently active model ID."""
    return _current_model


def getProvider() -> str:
    """Return the currently active provider."""
    return _current_provider


def _applyToMain():
    """Push the current model selection into main.py's module-level globals.

    This is the bridge between model_config and main.py — it overrides the
    hardcoded REVIEWER_MODEL, REVIEWER_PROVIDER, etc. so that all internal
    LLM calls (review, research, sync) use the selected model/provider.

    Called automatically by setModel() and setProvider().
    """
    try:
        import main as _main
        _main.REVIEWER_MODEL = _current_model
        _main.REVIEWER_PROVIDER = _current_provider
        _main.RESEARCH_MODEL = _current_model
        _main.RESEARCH_PROVIDER = _current_provider
    except ImportError:
        pass  # main.py not yet imported — will be applied at MCP server startup
