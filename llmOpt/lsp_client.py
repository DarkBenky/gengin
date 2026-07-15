"""
lsp_client.py — Thin clangd LSP client for the gengin MCP server.

Spawns clangd as a long-lived subprocess and communicates via JSON-RPC 2.0
over stdio with Content-Length framing.  Provides semantic code intelligence
(references, definition, rename, diagnostics, call hierarchy) that complements
getFunc.py's regex-based tools.

Usage:
    from lsp_client import getClient
    client = getClient(project_dir)
    refs = client.references("render/cpu/ray.c", 672, 10)
"""

import json
import os
import subprocess
import sys
import threading
import time

# ---------------------------------------------------------------------------
# JSON-RPC framing
# ---------------------------------------------------------------------------

def _encode_message(data: dict) -> bytes:
    body = json.dumps(data)
    header = f"Content-Length: {len(body)}\r\n\r\n"
    return header.encode() + body.encode()


def _decode_header(stream) -> int | None:
    """Read Content-Length header from a byte stream.  Returns length or None on EOF."""
    header = b""
    while not header.endswith(b"\r\n\r\n"):
        ch = stream.read(1)
        if not ch:
            return None
        header += ch
    for line in header.decode().split("\r\n"):
        if line.lower().startswith("content-length:"):
            return int(line.split(":", 1)[1].strip())
    return None


def _read_message(stream) -> dict | None:
    """Read one LSP message from a byte stream.  Returns parsed JSON or None on EOF."""
    length = _decode_header(stream)
    if length is None:
        return None
    body = stream.read(length)
    if not body:
        return None
    return json.loads(body)


# ---------------------------------------------------------------------------
# LSP Client
# ---------------------------------------------------------------------------

class LspClient:
    """Singleton clangd client.  Lazily starts clangd on first request."""

    def __init__(self, project_dir: str):
        self._project_dir = os.path.abspath(project_dir)
        self._proc: subprocess.Popen | None = None
        self._request_id = 0
        self._lock = threading.Lock()
        self._initialized = False
        self._open_files: set[str] = set()
        self._diagnostics_cache: dict[str, list[dict]] = {}

    # -- lifecycle --

    def start(self) -> bool:
        """Start clangd.  Returns True on success."""
        if self._proc is not None:
            return True

        compile_commands = os.path.join(self._project_dir, "compile_commands.json")
        if not os.path.exists(compile_commands):
            print(f"[lsp] No compile_commands.json in {self._project_dir} — "
                  f"run gen_compile_commands.py first", file=sys.stderr)
            return False

        try:
            self._proc = subprocess.Popen(
                ["clangd", f"--compile-commands-dir={self._project_dir}",
                 "--background-index", "--clang-tidy", "--limit-results=100"],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        except FileNotFoundError:
            print("[lsp] clangd not found — install clangd first", file=sys.stderr)
            return False

        # Initialize handshake
        init_params = {
            "processId": os.getpid(),
            "rootUri": f"file://{self._project_dir}",
            "capabilities": {
                "textDocument": {
                    "callHierarchy": {"dynamicRegistration": True},
                    "typeHierarchy": {"dynamicRegistration": True},
                    "documentSymbol": {
                        "hierarchicalDocumentSymbolSupport": True,
                    },
                },
            },
            "initializationOptions": {
                "clangdFileStatus": True,
            },
        }
        try:
            resp = self._request("initialize", init_params, timeout=15)
            if resp is None:
                self.stop()
                return False
            self._notify("initialized", {})
            self._initialized = True
            print(f"[lsp] clangd initialized (pid={self._proc.pid})", file=sys.stderr)
            return True
        except Exception as e:
            print(f"[lsp] clangd init failed: {e}", file=sys.stderr)
            self.stop()
            return False

    def stop(self):
        if self._proc:
            try:
                self._notify("exit", {})
            except Exception:
                pass
            try:
                self._proc.stdin.close()
            except Exception:
                pass
            try:
                self._proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait()
            self._proc = None
            self._initialized = False

    def ensureStarted(self) -> bool:
        if self._proc is None or self._proc.poll() is not None:
            self.stop()
            return self.start()
        return True

    # -- JSON-RPC core --

    def _nextId(self) -> int:
        with self._lock:
            self._request_id += 1
            return self._request_id

    def _notify(self, method: str, params: dict):
        """Send a notification (no response expected)."""
        if not self._proc or not self._proc.stdin:
            return
        msg = {"jsonrpc": "2.0", "method": method, "params": params}
        self._proc.stdin.write(_encode_message(msg))
        self._proc.stdin.flush()

    def _request(self, method: str, params: dict, timeout: float = 10.0) -> dict | None:
        """Send a request and wait for the response.  Returns result dict or None on error."""
        if not self._proc or not self._proc.stdin or not self._proc.stdout:
            return None

        req_id = self._nextId()
        msg = {"jsonrpc": "2.0", "id": req_id, "method": method, "params": params}

        with self._lock:
            self._proc.stdin.write(_encode_message(msg))
            self._proc.stdin.flush()

            deadline = time.time() + timeout
            while time.time() < deadline:
                if self._proc.poll() is not None:
                    return None  # process died
                resp = _read_message(self._proc.stdout)
                if resp is None:
                    return None
                if resp.get("id") == req_id:
                    if "error" in resp:
                        err = resp["error"]
                        print(f"[lsp] {method} error: {err.get('message', err)}", file=sys.stderr)
                        return None
                    return resp.get("result")
                # else: response for a different request id — ignore

        print(f"[lsp] {method} timed out after {timeout}s", file=sys.stderr)
        return None

    # -- helpers --

    def _resolveUri(self, rel_path: str) -> str:
        """Resolve a relative path to a file:// URI without sending didOpen.
        Used for read-only queries (references, definition, symbols) where
        clangd already knows the file from compile_commands.json."""
        abs_path = os.path.join(self._project_dir, rel_path)
        if not os.path.exists(abs_path):
            raise FileNotFoundError(f"File not found: {rel_path}")
        return f"file://{abs_path}"

    def _requireFile(self, rel_path: str) -> str:
        """Resolve rel_path to absolute, send didOpen and wait for clangd to
        finish parsing (by polling documentSymbol).  Also collects any
        publishDiagnostics notifications sent during parsing."""
        abs_path = os.path.join(self._project_dir, rel_path)
        if not os.path.exists(abs_path):
            raise FileNotFoundError(f"File not found: {rel_path}")
        if rel_path not in self._open_files:
            uri = f"file://{abs_path}"
            with open(abs_path, errors='replace') as f:
                text = f.read()
            self._notify("textDocument/didOpen", {
                "textDocument": {
                    "uri": uri,
                    "languageId": "c",
                    "version": 1,
                    "text": text,
                }
            })
            self._open_files.add(rel_path)
            # Poll documentSymbol until clangd finishes parsing. While waiting,
            # drain any publishDiagnostics notifications into the cache.
            deadline = time.time() + 8.0
            req_id = self._nextId()
            msg = {"jsonrpc": "2.0", "id": req_id, "method": "textDocument/documentSymbol",
                   "params": {"textDocument": {"uri": uri}}}
            while time.time() < deadline:
                with self._lock:
                    self._proc.stdin.write(_encode_message(msg))
                    self._proc.stdin.flush()
                # Read responses until we get one matching our request id
                inner_deadline = time.time() + 5.0
                got_symbols = False
                while time.time() < inner_deadline:
                    if self._proc.poll() is not None:
                        break
                    resp = _read_message(self._proc.stdout)
                    if resp is None:
                        break
                    rid = resp.get("id")
                    if rid == req_id:
                        got_symbols = "error" not in resp
                        break
                    elif rid is None:
                        # Notification — check for publishDiagnostics
                        method = resp.get("method", "")
                        if method == "textDocument/publishDiagnostics":
                            params = resp.get("params", {})
                            diag_uri = params.get("uri", "")
                            diags = params.get("diagnostics", [])
                            self._diagnostics_cache[diag_uri] = diags
                if got_symbols:
                    break
                time.sleep(0.5)
        return abs_path

    def _uriToRel(self, uri: str) -> str:
        """Convert file:// URI to a path relative to project_dir."""
        prefix = f"file://{self._project_dir}/"
        if uri.startswith(prefix):
            return uri[len(prefix):]
        if uri.startswith("file://"):
            return uri[7:]
        return uri

    # -- public API --

    def definition(self, rel_path: str, line: int, character: int) -> list[dict] | None:
        if not self.ensureStarted():
            return None
        abs_path = self._requireFile(rel_path)
        result = self._request("textDocument/definition", {
            "textDocument": {"uri": f"file://{abs_path}"},
            "position": {"line": line, "character": character},
        })
        if result is None:
            return None
        if isinstance(result, dict):
            result = [result]
        return result

    def references(self, rel_path: str, line: int, character: int,
                   include_declaration: bool = False) -> list[dict] | None:
        if not self.ensureStarted():
            return None
        abs_path = self._requireFile(rel_path)
        return self._request("textDocument/references", {
            "textDocument": {"uri": f"file://{abs_path}"},
            "position": {"line": line, "character": character},
            "context": {"includeDeclaration": include_declaration},
        })

    def rename(self, rel_path: str, line: int, character: int,
               new_name: str) -> dict | None:
        if not self.ensureStarted():
            return None
        abs_path = self._requireFile(rel_path)
        return self._request("textDocument/rename", {
            "textDocument": {"uri": f"file://{abs_path}"},
            "position": {"line": line, "character": character},
            "newName": new_name,
        })

    def prepareRename(self, rel_path: str, line: int, character: int) -> dict | None:
        if not self.ensureStarted():
            return None
        abs_path = self._requireFile(rel_path)
        return self._request("textDocument/prepareRename", {
            "textDocument": {"uri": f"file://{abs_path}"},
            "position": {"line": line, "character": character},
        })

    def documentSymbol(self, rel_path: str) -> list[dict] | None:
        if not self.ensureStarted():
            return None
        abs_path = self._requireFile(rel_path)
        return self._request("textDocument/documentSymbol", {
            "textDocument": {"uri": f"file://{abs_path}"},
        })

    def diagnostics(self, rel_path: str) -> list[dict] | None:
        """Return cached publishDiagnostics.  Opens the file if needed to
        trigger clangd parsing, then returns any diagnostics received."""
        if not self.ensureStarted():
            return None
        abs_path = os.path.join(self._project_dir, rel_path)
        uri = f"file://{abs_path}"
        if rel_path not in self._open_files:
            self._requireFile(rel_path)
        return self._diagnostics_cache.get(uri, [])

    def diagnosticsAll(self) -> dict[str, list[dict]] | None:
        if not self.ensureStarted():
            return None
        result: dict[str, list[dict]] = {}
        for rel_path in list(self._open_files):
            diags = self.diagnostics(rel_path)
            if diags:
                result[rel_path] = diags
        return result if result else None

    def implementation(self, rel_path: str, line: int, character: int) -> list[dict] | None:
        if not self.ensureStarted():
            return None
        abs_path = self._requireFile(rel_path)
        result = self._request("textDocument/implementation", {
            "textDocument": {"uri": f"file://{abs_path}"},
            "position": {"line": line, "character": character},
        })
        if result is None:
            return None
        if isinstance(result, dict):
            result = [result]
        return result

    def callHierarchy(self, rel_path: str, line: int, character: int,
                      direction: str = "incoming") -> list[dict] | None:
        if not self.ensureStarted():
            return None
        abs_path = self._requireFile(rel_path)
        items = self._request("textDocument/prepareCallHierarchy", {
            "textDocument": {"uri": f"file://{abs_path}"},
            "position": {"line": line, "character": character},
        })
        if not items or not isinstance(items, list) or len(items) == 0:
            return None
        item = items[0]
        method = "callHierarchy/incomingCalls" if direction == "incoming" else "callHierarchy/outgoingCalls"
        return self._request(method, {"item": item})


# ---------------------------------------------------------------------------
# Singleton
# ---------------------------------------------------------------------------

_client: LspClient | None = None


def getClient(project_dir: str | None = None) -> LspClient:
    """Get or create the singleton LspClient for the given project directory."""
    global _client
    if _client is None and project_dir is not None:
        _client = LspClient(project_dir)
    return _client


def resetClient():
    """Stop and reset the singleton client (useful for testing)."""
    global _client
    if _client:
        _client.stop()
        _client = None
