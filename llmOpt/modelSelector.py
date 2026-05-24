from openrouter import OpenRouter
import os

API_KEY = os.getenv("KEY")

def getResponse(prompt:str, model:str, provider:str) -> str:
    client = OpenRouter(api_key=API_KEY)
    response = client.chat.completions.create(
        model=model,
        provider=provider,
        messages=[{"role": "user", "content": prompt}]
    )
    return response.choices[0].message.content