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

def getResponse(prompt:str, model:str, provider:str) -> str:
    client = OpenRouter(api_key=API_KEY)
    res = client.chat.send(
        messages=[{"role": "user", "content": prompt}],
        model=model,
        provider={"order": [provider]} if provider else None,
    )
    return res.choices[0].message.content

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
    messages = [{"role": "user", "content": content}]

    def _post(token):
        payload = json.dumps({
            "messages": messages,
            "max_tokens": 32768,
            "stream": False,
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
    except Exception as e:
        if "401" in str(e):
            # Token expired — re-authenticate once
            token = _unsloth_bearer(host)
            resp = _post(token)
        else:
            raise

    with resp:
        data = json.loads(resp.read())

    content = data["choices"][0]["message"]["content"]
    content = re.sub(r"<think>.*?</think>\s*", "", content, flags=re.DOTALL)
    return content.strip()

if __name__ == "__main__":
    test_prompt = "What is the capital of France?"
    # print(getResponse(test_prompt, model="deepseek/deepseek-v4-pro", provider="deepseek"))
    print(getResponseQwen3_6(test_prompt, mode="instruct"))