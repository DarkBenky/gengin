#!/usr/bin/env python3
"""Narrow OpenRouter management/current-key API client.

Used by the supervisor to create, monitor, and delete budget-capped temporary
inference keys. Standard library only (urllib). Exceptions never contain
authorization headers or key material.

Base URL: https://openrouter.ai/api/v1
"""

import json
import os
import random
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone

# Overridable for testing or a proxied deployment; production default is OpenRouter.
BASE_URL = os.environ.get("OPENROUTER_API_BASE", "https://openrouter.ai/api/v1")
MAX_BODY = 1_000_000  # bounded response reads
KEY_NAME_PREFIX = "gengin-llmopt-"
DEFAULT_TIMEOUT = 30
MAX_RETRIES = 5
BACKOFF_BASE = 1.0
BACKOFF_MAX = 60.0


class OpenRouterError(Exception):
    """Base error. Never contains auth headers or key material."""


class OpenRouterAuthError(OpenRouterError):
    """401/403 — operator action required, do not retry."""


class OpenRouterClientError(OpenRouterError):
    """400 — request is malformed, do not retry."""


class OpenRouterServerError(OpenRouterError):
    """5xx / network — transient, retried with backoff."""


class OpenRouterRateLimited(OpenRouterError):
    """429 — retried honoring Retry-After."""


class OpenRouterTimeout(OpenRouterError):
    """Request timed out. For key creation this is AMBIGUOUS: the key may have
    been created. Reconcile by session name before creating another."""


def _utc_iso(dt):
    return dt.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


class OpenRouterClient:
    def __init__(self, management_key, timeout=DEFAULT_TIMEOUT):
        if not management_key:
            raise ValueError("management key is required")
        self._management_key = management_key
        self._timeout = timeout
    # -- internals ----------------------------------------------------------

    def _backoff(self, attempt):
        delay = min(BACKOFF_MAX, BACKOFF_BASE * (2 ** (attempt - 1)))
        return delay * (0.5 + random.random())  # jitter

    def _retry_delay(self, retry_after, attempt):
        if retry_after:
            try:
                return min(BACKOFF_MAX, max(0.0, float(retry_after)))
            except ValueError:
                pass
        return self._backoff(attempt)

    @staticmethod
    def _parse_json(raw):
        if not raw:
            return None
        try:
            return json.loads(raw)
        except (json.JSONDecodeError, UnicodeDecodeError):
            return None

    def _request(self, method, path, body=None, auth_key=None,
                 retries=MAX_RETRIES, retry_timeout=True):
        """Perform an HTTP request with bounded reads and strict JSON.

        Returns (status, parsed_json). 404 is returned, not raised. Raises
        sanitized OpenRouter* errors otherwise.
        """
        url = f"{BASE_URL}{path}"
        headers = {
            "Authorization": f"Bearer {auth_key or self._management_key}",
            "Content-Type": "application/json",
            "User-Agent": "gengin-llmopt-supervisor/1.0",
        }
        data = json.dumps(body).encode() if body is not None else None
        attempt = 0
        while True:
            attempt += 1
            req = urllib.request.Request(url, data=data, headers=headers, method=method)
            try:
                with urllib.request.urlopen(req, timeout=self._timeout) as resp:
                    return resp.status, self._parse_json(resp.read(MAX_BODY))
            except urllib.error.HTTPError as e:
                raw = e.read(MAX_BODY) if e.fp else b""
                status = e.code
                parsed = self._parse_json(raw)
                if status == 404:
                    return status, parsed
                retry_after = e.headers.get("Retry-After") if e.headers else None
                if status == 429:
                    if attempt > retries:
                        raise OpenRouterRateLimited("rate limited (429) after retries")
                    time.sleep(self._retry_delay(retry_after, attempt))
                    continue
                if status in (401, 403):
                    raise OpenRouterAuthError(f"auth failed ({status}); operator action required")
                if status == 400:
                    raise OpenRouterClientError("bad request (400)")
                if 500 <= status < 600:
                    if attempt > retries:
                        raise OpenRouterServerError(f"server error ({status}) after retries")
                    time.sleep(self._backoff(attempt))
                    continue
                raise OpenRouterError(f"unexpected status {status}")
            except (urllib.error.URLError, TimeoutError, OSError) as e:
                reason = getattr(e, "reason", e)
                is_timeout = isinstance(reason, (TimeoutError,)) or "timed out" in str(reason).lower()
                if is_timeout and not retry_timeout:
                    raise OpenRouterTimeout("request timed out")
                if attempt > retries:
                    if is_timeout:
                        raise OpenRouterTimeout("request timed out after retries")
                    raise OpenRouterServerError("network error after retries")
                time.sleep(self._backoff(attempt))
                continue

    # -- public API ---------------------------------------------------------

    def create_key(self, session_id, short_sha, limit_usd, expires_at):
        """Create a budget-capped temporary inference key.

        Returns (plaintext_key, key_hash). The plaintext key is shown only once
        and must never be logged. On timeout, raises OpenRouterTimeout
        (ambiguous — reconcile by name before creating another).
        """
        name = f"{KEY_NAME_PREFIX}{session_id}-{short_sha}"
        body = {
            "name": name,
            "limit": float(limit_usd),
            "limit_reset": None,
            "include_byok_in_limit": False,
            "expires_at": _utc_iso(expires_at),
        }
        status, parsed = self._request("POST", "/keys", body=body, retry_timeout=False)
        if status != 201:
            raise OpenRouterError(f"key creation returned {status}, expected 201")
        if not isinstance(parsed, dict):
            raise OpenRouterError("key creation returned non-JSON")
        key = parsed.get("key")
        data = parsed.get("data") or {}
        key_hash = data.get("hash")
        if not key or not key_hash:
            raise OpenRouterError("key creation response missing key/hash")
        returned_limit = data.get("limit")
        if returned_limit is not None and abs(float(returned_limit) - float(limit_usd)) > 0.01:
            raise OpenRouterError(f"key limit mismatch: requested {limit_usd}, got {returned_limit}")
        return key, key_hash

    def get_key_status(self, inference_key):
        """GET /key with the temporary inference key. Returns the data dict.

        A 404 (key deleted/expired) returns an empty dict.
        """
        status, parsed = self._request("GET", "/key", auth_key=inference_key)
        if status == 404:
            return {}
        if status != 200:
            raise OpenRouterError(f"key status returned {status}")
        if not isinstance(parsed, dict):
            raise OpenRouterError("key status returned non-JSON")
        return parsed.get("data") or {}

    def delete_key(self, key_hash):
        """DELETE /keys/<hash>. 200 {"deleted": true} or 404 both success.

        Returns True on success, raises on other errors.
        """
        encoded = urllib.parse.quote(key_hash, safe="")
        status, _parsed = self._request("DELETE", f"/keys/{encoded}")
        if status in (200, 404):
            return True
        raise OpenRouterError(f"key deletion returned {status}")

    def list_keys(self):
        """List keys (management). Returns the list of key objects."""
        status, parsed = self._request("GET", "/keys")
        if status != 200:
            raise OpenRouterError(f"list keys returned {status}")
        if not isinstance(parsed, dict):
            raise OpenRouterError("list keys returned non-JSON")
        data = parsed.get("data")
        return data if isinstance(data, list) else []

    def find_key_by_name(self, name):
        """Find a key by exact name (ambiguous-create reconciliation)."""
        for key_obj in self.list_keys():
            if key_obj.get("name") == name:
                return key_obj
        return None

    def get_credits(self):
        """Account credit summary (management key): total_credits/total_usage."""
        status, parsed = self._request("GET", "/credits")
        if status != 200:
            raise OpenRouterError(f"credits endpoint returned {status}")
        if not isinstance(parsed, dict):
            raise OpenRouterError("credits returned non-JSON")
        return parsed.get("data") or {}

    def delete_stale_keys(self, prefix=KEY_NAME_PREFIX):
        """Delete all keys whose name starts with prefix. Returns count.

        Never deletes keys outside the owned prefix.
        """
        deleted = 0
        for key_obj in self.list_keys():
            name = key_obj.get("name") or ""
            if not name.startswith(prefix):
                continue
            key_hash = key_obj.get("hash") or key_obj.get("id")
            if not key_hash:
                continue
            if self.delete_key(key_hash):
                deleted += 1
        return deleted


def model_available(model_id, timeout=DEFAULT_TIMEOUT):
    """Check the public model list for an exact id.

    Returns True/False, or None when the list cannot be fetched (the caller
    decides whether that is fatal).
    """
    try:
        req = urllib.request.Request(
            f"{BASE_URL}/models",
            headers={"User-Agent": "gengin-llmopt-supervisor/1.0"})
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read(MAX_BODY))
        return any(m.get("id") == model_id for m in data.get("data", []))
    except (urllib.error.URLError, TimeoutError, OSError, json.JSONDecodeError,
            UnicodeDecodeError):
        return None


def key_expires_at(session_timeout_seconds, key_expiry_grace_seconds):
    """Compute the key expiration: session deadline + grace, in UTC."""
    from datetime import timedelta
    return utc_now() + timedelta(seconds=session_timeout_seconds + key_expiry_grace_seconds)


def utc_now():
    return datetime.now(timezone.utc)
