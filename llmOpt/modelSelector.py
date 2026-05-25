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

if __name__ == "__main__":
    test_prompt = "What is the capital of France?"
    print(getResponse(test_prompt, model="deepseek/deepseek-v4-pro", provider="deepseek"))