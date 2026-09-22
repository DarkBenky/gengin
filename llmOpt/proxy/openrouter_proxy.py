#!/usr/bin/env python3
"""OpenRouter filtering proxy for the gengin optimizer.

Sits between Hermes (and any other local tool) and OpenRouter and makes the
cheap-provider problem impossible by injection, not by trust:

  - adds `provider.quantizations` (default: int8/fp6/fp8/fp16/bf16/fp32/unknown)
    so low-quantization (q4/q3/q2) endpoints are not silently selected;
  - adds `provider.allow_fallbacks: false` by default, so OpenRouter does not
    walk through provider fallbacks on a failure; the proxy itself retries
    exactly once without any injection, then backs off per model;
  - appends `:floor` (price-sorted routing) to the model id unless the caller
    already chose a routing variant.

Skip rules — the request is passed through untouched when the caller clearly
made an explicit choice:

  - model id carries a suffix outside the known OpenRouter variant set
    (e.g. ``xiaomi/mimo-v2.6-flash:xiaomi``);
  - model id carries ``:free`` (already the cheapest cluster; a quantization
    filter would exclude its endpoints);
  - the request pins providers itself (``provider.only`` / ``provider.order``);
  - the body has no single string ``model`` (multi-model fallback arrays).

Standard library only (http.server + urllib), streaming-safe: response bodies
are relayed with chunked transfer encoding as they arrive, so SSE completions
are not buffered. Never logs bodies, headers or keys.

Env knobs (all optional):
  GENGIN_PROXY_PORT=8787
  GENGIN_PROXY_BIND=127.0.0.1
  GENGIN_PROXY_UPSTREAM=https://openrouter.ai
  GENGIN_PROXY_QUANTIZATIONS=int8,fp6,fp8,fp16,bf16,fp32,unknown
  GENGIN_PROXY_FLOOR=1
  GENGIN_PROXY_ALLOW_FALLBACKS=0
  GENGIN_PROXY_FAIL_THRESHOLD=3
  GENGIN_PROXY_COOLDOWN_SECONDS=3600
  GENGIN_PROXY_TIMEOUT=300
  GENGIN_PROXY_LOG=-            # "-" = stderr, or an absolute file path
"""

import copy
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(os.environ.get("GENGIN_PROXY_PORT", "8787") or 8787)
BIND = os.environ.get("GENGIN_PROXY_BIND", "127.0.0.1")
UPSTREAM = os.environ.get("GENGIN_PROXY_UPSTREAM", "https://openrouter.ai").rstrip("/")
QUANTIZATIONS = [
    q.strip()
    for q in os.environ.get(
        "GENGIN_PROXY_QUANTIZATIONS", "int8,fp6,fp8,fp16,bf16,fp32,unknown"
    ).split(",")
    if q.strip()
]
ENABLE_FLOOR = os.environ.get("GENGIN_PROXY_FLOOR", "1") != "0"
ALLOW_FALLBACKS = os.environ.get("GENGIN_PROXY_ALLOW_FALLBACKS", "0") != "0"
FAIL_THRESHOLD = int(os.environ.get("GENGIN_PROXY_FAIL_THRESHOLD", "3") or 3)
COOLDOWN_SECONDS = float(os.environ.get("GENGIN_PROXY_COOLDOWN_SECONDS", "3600") or 3600)
TIMEOUT = float(os.environ.get("GENGIN_PROXY_TIMEOUT", "300") or 300)
LOG_TARGET = os.environ.get("GENGIN_PROXY_LOG", "-")
MAX_BODY = 64 * 1024 * 1024

# Only these suffixes are OpenRouter variants. Anything else in a model id is
# treated as a caller-chosen pin and the request passes through untouched.
CATALOG_VARIANTS = {"free", "batch", "thinking", "extended"}
ROUTING_VARIANTS = {"nitro", "floor", "exacto", "online"}
KNOWN_VARIANTS = CATALOG_VARIANTS | ROUTING_VARIANTS

_lock = threading.Lock()
_model_state = {}  # model -> {"fails": int, "skip_until": float}
_stats = {"requests": 0, "injected": 0, "skipped": 0, "retries": 0, "failures": 0}


def _log_line(line):
    stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    text = f"{stamp} {line}"
    if LOG_TARGET == "-":
        print(text, file=sys.stderr, flush=True)
        return
    try:
        with open(LOG_TARGET, "a") as fh:
            fh.write(text + "\n")
    except OSError:
        print(text, file=sys.stderr, flush=True)


def _model_status(model):
    with _lock:
        return dict(_model_state.get(model) or {"fails": 0, "skip_until": 0.0})


def _record_result(model, ok):
    with _lock:
        state = _model_state.setdefault(model, {"fails": 0, "skip_until": 0.0})
        if ok:
            state["fails"] = 0
            return
        state["fails"] += 1
        if state["fails"] >= FAIL_THRESHOLD:
            state["skip_until"] = time.time() + COOLDOWN_SECONDS
            state["fails"] = 0


def _parse_model(model):
    """(base, suffixes, has_unknown_suffix) for a model id."""
    parts = model.split(":")
    if len(parts) == 1:
        return model, [], False
    base, suffixes = parts[0], parts[1:]
    unknown = any(s and s not in KNOWN_VARIANTS for s in suffixes)
    return base, suffixes, unknown


def _status_of(resp):
    """HTTP status for a urlopen result or an urllib HTTPError."""
    status = getattr(resp, "status", None)
    if status is None:
        status = getattr(resp, "code", None)
    return int(status or 0)


def _should_skip(body, model):
    if isinstance(body.get("models"), list) and body["models"]:
        return "multi-model"
    _, suffixes, unknown = _parse_model(model)
    if unknown:
        return "unknown-suffix"
    if "free" in suffixes:
        return "free-variant"
    provider = body.get("provider")
    if isinstance(provider, dict):
        if provider.get("only") or provider.get("order"):
            return "provider-pinned"
    return ""


def _apply_filters(body, model):
    """Mutate body in place; return (applied_fields, skip_reason or "")."""
    skip = _should_skip(body, model)
    if skip:
        return [], skip
    status = _model_status(model)
    if time.time() < status["skip_until"]:
        return [], "cooldown"

    applied = []
    provider = body.get("provider")
    provider = dict(provider) if isinstance(provider, dict) else {}
    if not provider.get("quantizations"):
        provider["quantizations"] = list(QUANTIZATIONS)
        applied.append("quantizations")
    if not ALLOW_FALLBACKS and "allow_fallbacks" not in provider:
        provider["allow_fallbacks"] = False
        applied.append("allow_fallbacks")
    if provider:
        body["provider"] = provider

    _, suffixes, _ = _parse_model(model)
    if ENABLE_FLOOR and not any(s in ROUTING_VARIANTS for s in suffixes):
        body["model"] = model + ":floor"
        applied.append("floor")
    return applied, ""


class ProxyHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "gengin-openrouter-proxy"

    # -- helpers -----------------------------------------------------------

    def log_message(self, fmt, *args):  # silence default stderr noise
        return

    def _read_body(self):
        length = self.headers.get("Content-Length")
        if length is None:
            return b""
        try:
            n = int(length)
        except ValueError:
            return b""
        if n <= 0 or n > MAX_BODY:
            return b""
        return self.rfile.read(n)

    def _forward_headers(self):
        headers = {}
        for key, value in self.headers.items():
            if key.lower() in ("host", "content-length", "accept-encoding"):
                continue
            headers[key] = value
        # Keep responses plain so chunked relay is byte-exact.
        headers["Accept-Encoding"] = "identity"
        return headers

    def _send_upstream_status(self, status, headers):
        self.send_response_only(status)
        for key, value in headers.items():
            if key.lower() in (
                "connection", "keep-alive", "proxy-authenticate",
                "proxy-authorization", "te", "trailers", "transfer-encoding",
                "content-length", "upgrade",
            ):
                continue
            self.send_header(key, value)
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

    def _write_chunk(self, chunk):
        self.wfile.write(b"%X\r\n" % len(chunk))
        self.wfile.write(chunk)
        self.wfile.write(b"\r\n")

    def _relay_body(self, resp, buffered=None):
        try:
            if buffered is not None:
                if buffered:
                    self._write_chunk(buffered)
                self.wfile.write(b"0\r\n\r\n")
                return True
            reader = getattr(resp, "read1", None) or resp.read
            while True:
                chunk = reader(65536)
                if not chunk:
                    break
                self._write_chunk(chunk)
            self.wfile.write(b"0\r\n\r\n")
            return True
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            self.close_connection = True
            return False

    def _send_json(self, status, payload):
        data = json.dumps(payload, indent=2).encode() + b"\n"
        self.send_response_only(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _upstream_request(self, body, headers):
        url = UPSTREAM + self.path
        req = urllib.request.Request(
            url, data=body if body is not None else None, method=self.command
        )
        for key, value in headers.items():
            req.add_header(key, value)
        return urllib.request.urlopen(req, timeout=TIMEOUT)

    # -- request routing ---------------------------------------------------

    def do_GET(self):
        if self.path == "/health":
            self._send_json(200, {"ok": True, "upstream": UPSTREAM, "port": PORT})
            return
        if self.path == "/status":
            with _lock:
                models = {
                    model: dict(state) for model, state in _model_state.items()
                }
                stats = dict(_stats)
            self._send_json(200, {
                "config": {
                    "upstream": UPSTREAM,
                    "quantizations": QUANTIZATIONS,
                    "floor": ENABLE_FLOOR,
                    "allow_fallbacks": ALLOW_FALLBACKS,
                    "fail_threshold": FAIL_THRESHOLD,
                    "cooldown_seconds": COOLDOWN_SECONDS,
                },
                "stats": stats,
                "models": models,
            })
            return
        self._passthrough()

    def do_POST(self):
        with _lock:
            _stats["requests"] += 1
        raw = self._read_body()
        headers = self._forward_headers()

        try:
            body = json.loads(raw) if raw else {}
        except (json.JSONDecodeError, UnicodeDecodeError):
            self._passthrough(raw=raw, headers=headers)
            return

        model = body.get("model") if isinstance(body, dict) else None
        if not isinstance(model, str) or not model:
            self._passthrough(raw=raw, headers=headers)
            return

        original = copy.deepcopy(body)
        applied, skip_reason = _apply_filters(body, model)
        started = time.monotonic()

        try:
            upstream = self._upstream_request(json.dumps(body).encode(), headers)
        except urllib.error.HTTPError as exc:
            upstream = exc
        except OSError as exc:
            _log_line(
                f"POST {self.path} model={model} skip={skip_reason or '-'} "
                f"error={type(exc).__name__}"
            )
            self._send_json(502, {"error": {"message": f"proxy: upstream error: {exc}"}})
            return

        status = _status_of(upstream)
        retried = False
        buffered = None

        # Fail-open retry: the injected constraints can exclude every endpoint
        # of a model (e.g. closed models with no quantization metadata). Drop
        # them once; the error body is buffered so it can still be relayed.
        if applied and status in (400, 404):
            try:
                buffered = upstream.read()
            except OSError:
                buffered = b""
            text = buffered.decode("utf-8", "ignore").lower()
            retryable = status == 404 or any(
                token in text
                for token in ("provider", "endpoint", "quantization", "no allowed")
            )
            if retryable:
                with _lock:
                    _stats["retries"] += 1
                _record_result(model, ok=False)
                try:
                    upstream.close()
                except OSError:
                    pass
                try:
                    upstream = self._upstream_request(
                        json.dumps(original).encode(), headers
                    )
                except urllib.error.HTTPError as exc:
                    upstream = exc
                except OSError as exc:
                    _log_line(
                        f"POST {self.path} model={model} retry error={type(exc).__name__}"
                    )
                    self._send_json(502, {"error": {"message": f"proxy: upstream error: {exc}"}})
                    return
                retried = True
                buffered = None
                status = _status_of(upstream)

        if applied and 200 <= status < 300:
            _record_result(model, ok=True)

        streamed = "event-stream" in (upstream.headers.get("Content-Type") or "")
        elapsed_ms = int((time.monotonic() - started) * 1000)
        if applied or skip_reason:
            with _lock:
                _stats["injected" if applied else "skipped"] += 1
        _log_line(
            f"POST {self.path} model={model} inject={'+'.join(applied) or '-'} "
            f"skip={skip_reason or '-'} status={status} stream={1 if streamed else 0} "
            f"retry={1 if retried else 0} ms={elapsed_ms}"
        )

        try:
            self._send_upstream_status(_status_of(upstream), upstream.headers)
            self._relay_body(upstream, buffered)
        finally:
            upstream.close()

    do_PUT = do_POST
    do_PATCH = do_POST

    def _passthrough(self, raw=None, headers=None):
        if raw is None:
            raw = self._read_body()
        if headers is None:
            headers = self._forward_headers()
        try:
            upstream = self._upstream_request(raw if raw else None, headers)
        except urllib.error.HTTPError as exc:
            upstream = exc
        except OSError as exc:
            self._send_json(502, {"error": {"message": f"proxy: upstream error: {exc}"}})
            return
        try:
            self._send_upstream_status(_status_of(upstream), upstream.headers)
            self._relay_body(upstream)
        finally:
            upstream.close()


def main():
    server = ThreadingHTTPServer((BIND, PORT), ProxyHandler)
    server.daemon_threads = True
    _log_line(f"listening on http://{BIND}:{PORT} -> {UPSTREAM} (port {PORT})")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
