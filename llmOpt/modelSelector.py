"""
modelSelector.py — LLM call dispatcher for the gengin optimization harness.

Supports three backends:
  openrouter — OpenRouter API (default). COST_FIRST=True selects the cheapest
               provider via provider.sort:"price". COST_FIRST=False uses the
               explicit provider pin.
  deepseek   — DeepSeek direct API (https://api.deepseek.com). Requires
               DEEPSEEK_API_KEY in .env.

Globals (set by model_config._applyToMain or directly):
  COST_FIRST  — bool, select cheapest OpenRouter provider (default True)
  BACKEND     — "openrouter" | "deepseek" (default "openrouter")

Also provides:
  getResponseOllama — local Ollama backend
  getResponseQwen3_6 — Unsloth Studio local backend
"""
from openrouter import OpenRouter
import os

def _load_env(path: str):
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    k, _, v = line.partition("=")
                    os.environ.setdefault(k.strip(), v.strip())
    except FileNotFoundError:
        pass

_load_env(os.path.join(os.path.dirname(__file__), ".env"))
API_KEY = os.getenv("KEY")
DEEPSEEK_API_KEY = os.getenv("DEEPSEEK_API_KEY", "")

# Set by model_config._applyToMain(); these control dispatch.
COST_FIRST = True
BACKEND = "openrouter"


def getResponse(
    prompt: str,
    model: str,
    provider: str | None = None,
    reasoning_effort: str | None = "xhigh",
) -> str:
    """
    Send a chat request through the configured backend.

    When BACKEND="deepseek", dispatches to DeepSeek direct API regardless
    of the provider arg (provider arg is ignored in that case).

    When BACKEND="openrouter":
      COST_FIRST=True  — uses provider.sort:"price" (cheapest provider)
      COST_FIRST=False — uses provider.order:[provider] (explicit pin)

    reasoning_effort controls thinking mode depth:
      "xhigh" = max thinking (best for complex code optimization tasks)
      "high"  = high thinking
      "medium"/"low" = mapped to "high"
      None    = disable thinking (model default)
    """
    if BACKEND == "deepseek":
        return _getResponseDeepSeek(prompt, model, reasoning_effort)

    # OpenRouter path
    from openrouter import OpenRouter
    from openrouter import components

    client = OpenRouter(api_key=API_KEY)

    if COST_FIRST:
        prov_cfg = {"sort": "price"}
    else:
        prov_cfg = {"order": [provider]} if provider else None

    kwargs = {
        "messages": [{"role": "user", "content": prompt}],
        "model": model,
        "provider": prov_cfg,
    }
    if reasoning_effort is not None:
        kwargs["reasoning"] = components.Reasoning(effort=reasoning_effort)

    res = client.chat.send(**kwargs)
    return res.choices[0].message.content


def _getResponseDeepSeek(
    prompt: str,
    model: str,
    reasoning_effort: str | None = "xhigh",
) -> str:
    """Call DeepSeek direct API (OpenAI-compatible, https://api.deepseek.com).

    Model strings like "deepseek/deepseek-v4-flash-0731" are stripped to the
    API model id (e.g. "deepseek-v4-flash"). The -0731 version suffix is
    handled by DeepSeek's own versioning; the direct API uses "deepseek-v4-flash".
    """
    import urllib.request, json

    if not DEEPSEEK_API_KEY:
        raise RuntimeError(
            "DEEPSEEK_API_KEY not set. Add DEEPSEEK_API_KEY=... to llmOpt/.env"
        )

    # Strip OpenRouter prefix and version suffixes to get the API model id.
    api_model = model.split("/")[-1]
    # Remove version suffixes like -0731 to get the base API model id.
    api_model = api_model.split("-07")[0] if "-07" in api_model else api_model

    body: dict = {
        "model": api_model,
        "messages": [{"role": "user", "content": prompt}],
        "thinking": {"type": "enabled"},
    }
    if reasoning_effort:
        body["reasoning_effort"] = reasoning_effort

    req = urllib.request.Request(
        "https://api.deepseek.com/chat/completions",
        data=json.dumps(body).encode(),
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {DEEPSEEK_API_KEY}",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=300) as resp:
        data = json.loads(resp.read())
    return data["choices"][0]["message"]["content"]

def getResponseOllama(prompt: str, model: str) -> str:
    import urllib.request
    import json
    payload = json.dumps({
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
        "stream": False,
    }).encode()
    req = urllib.request.Request(
        "http://localhost:11434/api/chat",
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req) as resp:
        data = json.loads(resp.read())
    return data["message"]["content"]

_UNSLOTH_TOKEN: str = ""

def _unsloth_bearer(host: str) -> str:
    """Authenticate with Unsloth Studio desktop-login and return a Bearer token."""
    import urllib.request, json as _json, pathlib, subprocess
    global _UNSLOTH_TOKEN

    secret_file = pathlib.Path.home() / ".unsloth" / "studio" / "auth" / ".desktop_secret"
    if not secret_file.exists():
        # provision desktop auth if not yet done
        unsloth_bin = pathlib.Path.home() / ".unsloth" / "studio" / "unsloth_studio" / "bin" / "unsloth"
        subprocess.run([str(unsloth_bin), "studio", "provision-desktop-auth"], check=True)

    secret = secret_file.read_text().strip()
    req = urllib.request.Request(
        f"{host}/api/auth/desktop-login",
        data=_json.dumps({"secret": secret}).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=10) as r:
        tok = _json.loads(r.read())
    _UNSLOTH_TOKEN = tok["access_token"]
    return _UNSLOTH_TOKEN


def getResponseQwen3_6(
    prompt: str,
    mode: str = "coding",   # "coding" | "general" | "instruct"
    host: str = "http://localhost:8888",
    on_token=None,
    system_prompt: str = None,
) -> str:
    """
    Call the Unsloth Studio server (localhost:8888) with Unsloth-recommended parameters.

    Modes:
      coding   — thinking mode, precise coding  (temp=0.6, top_p=0.95, top_k=20)
      general  — thinking mode, general tasks   (temp=1.0, top_p=0.95, top_k=20)
      instruct — non-thinking, general tasks    (temp=0.7, top_p=0.8,  top_k=20)

    Start Unsloth Studio:
      HF_HOME=/media/user/2TB/huggingface_cache unsloth studio -H 127.0.0.1 -p 8888
    """
    import urllib.request
    import json
    import re

    PRESETS = {
        "coding": dict(
            thinking=True,
            temperature=0.6,
            top_p=0.95,
            top_k=20,
            min_p=0.0,
            presence_penalty=0.0,
            repeat_penalty=1.0,
        ),
        "general": dict(
            thinking=True,
            temperature=1.0,
            top_p=0.95,
            top_k=20,
            min_p=0.0,
            presence_penalty=1.5,
            repeat_penalty=1.0,
        ),
        "instruct": dict(
            thinking=False,
            temperature=0.7,
            top_p=0.8,
            top_k=20,
            min_p=0.0,
            presence_penalty=1.5,
            repeat_penalty=1.0,
        ),
    }

    if mode not in PRESETS:
        raise ValueError(f"mode must be one of {list(PRESETS)}, got {mode!r}")

    p = dict(PRESETS[mode])
    thinking = p.pop("thinking")

    content = prompt if thinking else f"/no_think {prompt}"
    messages = []
    if system_prompt:
        messages.append({"role": "system", "content": system_prompt})
    messages.append({"role": "user", "content": content})

    def _post(token):
        payload = json.dumps({
            "messages": messages,
            "max_tokens": 64_000,
            "stream": True,
            **p,
        }).encode()
        req = urllib.request.Request(
            f"{host}/v1/chat/completions",
            data=payload,
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {token}"},
            method="POST",
        )
        return urllib.request.urlopen(req, timeout=300)

    token = _UNSLOTH_TOKEN or _unsloth_bearer(host)
    try:
        resp = _post(token)
    except urllib.error.HTTPError as e:
        if e.code == 401:
            token = _unsloth_bearer(host)
            resp = _post(token)
        else:
            raise

    chunks = []
    in_think = False
    with resp:
        for raw_line in resp:
            line = raw_line.decode().strip()
            if not line.startswith("data:"):
                continue
            chunk = line[5:].strip()
            if chunk == "[DONE]":
                break
            try:
                delta = json.loads(chunk)["choices"][0]["delta"].get("content", "")
            except (KeyError, IndexError, json.JSONDecodeError):
                continue
            if not delta:
                continue
            chunks.append(delta)
            if "<think>" in delta:
                in_think = True
            if not in_think:
                print(delta, end="", flush=True)
                if on_token:
                    on_token(delta)
            if "</think>" in delta:
                in_think = False
    print()
    content = "".join(chunks)
    return re.sub(r"<think>.*?</think>\s*", "", content, flags=re.DOTALL).strip()

if __name__ == "__main__":
    test_prompt = "What is the capital of France?"
    # print(getResponse(test_prompt, model="deepseek/deepseek-v4-pro", provider="deepseek"))
    print(getResponseQwen3_6(test_prompt, mode="instruct"))