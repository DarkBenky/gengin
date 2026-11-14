#!/usr/bin/env python3
"""Unit tests for llmOpt/small_model.py (local-only small-model delegation).

Everything is mocked at the urllib boundary (no network), plus one subprocess
test that proves the MCP server registers the tool only on local runs.
Run with: python3 -m unittest discover -s llmOpt/tests -v
"""

import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
import urllib.error
from unittest import mock

_LLMOPT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _LLMOPT_DIR)

import small_model as sm  # noqa: E402

MODULE_DEFAULTS = (
    "BASE_URL", "MODEL", "TIMEOUT_SECONDS", "MAX_TASK_CHARS",
    "MAX_CONTEXT_CHARS", "DEFAULT_MAX_TOKENS", "MAX_TOKENS_CAP", "LOG_PATH",
)


class FakeResponse:
    def __init__(self, body):
        self._body = (body if isinstance(body, bytes)
                      else json.dumps(body).encode("utf-8"))

    def read(self, size=-1):
        if size is None or size < 0:
            return self._body
        return self._body[:size]

    def __enter__(self):
        return self

    def __exit__(self, *exc_info):
        return False


class Recorder:
    """urlopen stand-in: pops scripted responses, records request bodies."""

    def __init__(self, *responses):
        self._responses = list(responses)
        self.requests = []

    def __call__(self, request, timeout=None):
        self.requests.append(request)
        if not self._responses:
            raise AssertionError("unexpected upstream request")
        result = self._responses.pop(0)
        if isinstance(result, BaseException):
            raise result
        return result

    @property
    def bodies(self):
        return [json.loads(r.data.decode("utf-8")) for r in self.requests]

    @property
    def urls(self):
        return [r.full_url for r in self.requests]


def http_error(code, body=b'{"error":"boom"}'):
    return urllib.error.HTTPError(
        "http://small.test/v1/chat/completions", code, "error", {},
        io.BytesIO(body))


def chat_reply(content, finish_reason="stop", usage=None):
    payload = {
        "choices": [{"index": 0, "finish_reason": finish_reason,
                     "message": {"role": "assistant", "content": content}}],
    }
    if usage is not None:
        payload["usage"] = usage
    return FakeResponse(payload)


class SmallModelTestBase(unittest.TestCase):
    def setUp(self):
        self._saved = {name: getattr(sm, name) for name in MODULE_DEFAULTS}
        self.addCleanup(self._restore)
        env_patch = mock.patch.dict(
            os.environ, {"GENGIN_SMALL_MODEL_ENABLE": "1"}, clear=False)
        env_patch.start()
        self.addCleanup(env_patch.stop)
        self.tmpdir = tempfile.mkdtemp(prefix="small-model-test-")
        sm.BASE_URL = "http://small.test/v1"
        sm.MODEL = "qwen2.5-coder-7b"
        sm.TIMEOUT_SECONDS = 5.0
        sm.MAX_TASK_CHARS = 4000
        sm.MAX_CONTEXT_CHARS = 1000
        sm.DEFAULT_MAX_TOKENS = 600
        sm.LOG_PATH = os.path.join(self.tmpdir, "small_model.jsonl")

    def _restore(self):
        for name, value in self._saved.items():
            setattr(sm, name, value)

    def call(self, recorder, **kwargs):
        with mock.patch("urllib.request.urlopen", recorder):
            return sm.delegate(**kwargs)

    def log_lines(self):
        if not os.path.exists(sm.LOG_PATH):
            return []
        with open(sm.LOG_PATH, encoding="utf-8") as fh:
            return [json.loads(line) for line in fh if line.strip()]


class EnableGateTests(SmallModelTestBase):
    def test_enabled_by_the_local_flag(self):
        self.assertTrue(sm.is_enabled())

    def test_disabled_without_the_flag(self):
        recorder = Recorder()
        with mock.patch.dict(os.environ, {"GENGIN_SMALL_MODEL_ENABLE": ""}):
            self.assertFalse(sm.is_enabled())
            result = self.call(recorder, task="rename a to b")
        self.assertIn("error", result)
        self.assertIn("local runs", result["error"])
        self.assertEqual(recorder.requests, [])

    def test_disabled_gate_is_logged_nothing(self):
        with mock.patch.dict(os.environ, {"GENGIN_SMALL_MODEL_ENABLE": "0"}):
            self.call(Recorder(), task="x")
        self.assertEqual(self.log_lines(), [])


class ArgumentTests(SmallModelTestBase):
    def test_task_is_required(self):
        recorder = Recorder()
        result = self.call(recorder, task="   ")
        self.assertIn("error", result)
        self.assertIn("task is required", result["error"])
        self.assertEqual(recorder.requests, [])

    def test_task_over_cap_is_rejected_not_truncated(self):
        recorder = Recorder()
        sm.MAX_TASK_CHARS = 10
        result = self.call(recorder, task="x" * 11)
        self.assertIn("error", result)
        self.assertIn("limit is 10", result["error"])
        self.assertEqual(recorder.requests, [])

    def test_max_tokens_out_of_range(self):
        for bad in (-3, sm.MAX_TOKENS_CAP + 1):
            with self.subTest(bad=bad):
                result = self.call(Recorder(), task="t", max_tokens=bad)
                self.assertIn("error", result)
                self.assertIn("max_tokens", result["error"])

    def test_max_tokens_non_integer(self):
        result = self.call(Recorder(), task="t", max_tokens="lots")
        self.assertIn("must be an integer", result["error"])

    def test_default_max_tokens_and_override(self):
        recorder = Recorder(chat_reply("a"), chat_reply("b"))
        sm.DEFAULT_MAX_TOKENS = 321
        self.call(recorder, task="t")
        self.call(recorder, task="t", max_tokens=42)
        self.assertEqual([b["max_tokens"] for b in recorder.bodies], [321, 42])


class RequestShapeTests(SmallModelTestBase):
    def test_body_and_url(self):
        recorder = Recorder(chat_reply("out"))
        result = self.call(recorder, task="Rename a to b",
                           context="int a = 1;")
        self.assertEqual(recorder.urls,
                         ["http://small.test/v1/chat/completions"])
        body = recorder.bodies[0]
        self.assertEqual(body["model"], "qwen2.5-coder-7b")
        self.assertEqual(body["temperature"], 0.1)
        self.assertFalse(body["stream"])
        self.assertEqual(body["max_tokens"], 600)
        self.assertEqual([m["role"] for m in body["messages"]],
                         ["system", "user"])
        self.assertIn("helper model", body["messages"][0]["content"])
        self.assertIn("INSUFFICIENT CONTEXT",
                      body["messages"][0]["content"])
        user = body["messages"][1]["content"]
        self.assertTrue(user.startswith("Rename a to b"))
        self.assertIn("<<<\nint a = 1;\n>>>", user)
        self.assertEqual(result["output"], "out")
        self.assertEqual(result["finish_reason"], "stop")
        self.assertIn("ms", result)

    def test_no_context_means_no_context_block(self):
        recorder = Recorder(chat_reply("out"))
        self.call(recorder, task="just the task")
        self.assertEqual(recorder.bodies[0]["messages"][1]["content"],
                         "just the task")

    def test_context_is_truncated_and_reported(self):
        recorder = Recorder(chat_reply("out"))
        sm.MAX_CONTEXT_CHARS = 10
        result = self.call(recorder, task="t", context="0123456789ABCDEF")
        self.assertTrue(result["context_truncated"])
        self.assertIn("<<<\n0123456789\n>>>",
                      recorder.bodies[0]["messages"][1]["content"])

    def test_request_timeout_uses_module_budget(self):
        calls = {}

        def recorder(request, timeout=None):
            calls["timeout"] = timeout
            return chat_reply("ok")

        sm.TIMEOUT_SECONDS = 7.5
        with mock.patch("urllib.request.urlopen", recorder):
            sm.delegate(task="t")
        self.assertEqual(calls["timeout"], 7.5)


class ResponseTests(SmallModelTestBase):
    def test_usage_and_length_are_reported(self):
        recorder = Recorder(chat_reply(
            "partial", finish_reason="length",
            usage={"prompt_tokens": 12, "completion_tokens": 3}))
        result = self.call(recorder, task="t", max_tokens=3)
        self.assertTrue(result["truncated"])
        self.assertEqual(result["finish_reason"], "length")
        self.assertEqual(result["tokens"], {"prompt": 12, "completion": 3})

    def test_insufficient_context_marker(self):
        recorder = Recorder(chat_reply("  INSUFFICIENT CONTEXT \n"))
        result = self.call(recorder, task="t")
        self.assertTrue(result["insufficient_context"])

    def test_null_content_becomes_empty_output(self):
        recorder = Recorder(FakeResponse(
            {"choices": [{"finish_reason": "stop",
                          "message": {"role": "assistant",
                                      "content": None}}]}))
        result = self.call(recorder, task="t")
        self.assertEqual(result["output"], "")

    def test_http_error_is_returned_and_logged(self):
        result = self.call(Recorder(http_error(503, b'{"error":"busy"}')),
                           task="t")
        self.assertIn("HTTP 503", result["error"])
        self.assertIn("busy", result["error"])
        self.assertEqual(self.log_lines()[-1]["event"], "http_error")

    def test_unreachable_hint(self):
        result = self.call(Recorder(urllib.error.URLError("refused")), task="t")
        self.assertIn("not reachable", result["error"])
        self.assertIn("llama-server-zeta", result["error"])
        self.assertEqual(self.log_lines()[-1]["event"], "unreachable")

    def test_timeout_has_its_own_message(self):
        sm.TIMEOUT_SECONDS = 2.5
        wrapped = urllib.error.URLError(TimeoutError("timed out"))
        for failure in (wrapped, TimeoutError("timed out")):
            with self.subTest(failure=type(failure).__name__):
                result = self.call(Recorder(failure), task="t")
                self.assertIn("timed out after 2.5s", result["error"])
                self.assertIn("GENGIN_SMALL_MODEL_TIMEOUT_SECONDS",
                              result["error"])
                self.assertNotIn("not reachable", result["error"])
                entry = self.log_lines()[-1]
                self.assertEqual(entry["event"], "timeout")
                self.assertEqual(entry["timeout_s"], 2.5)

    def test_invalid_json(self):
        result = self.call(Recorder(FakeResponse(b"<html>no</html>")),
                           task="t")
        self.assertIn("unexpected response", result["error"])
        self.assertEqual(self.log_lines()[-1]["event"], "bad_response")

    def test_unexpected_shape(self):
        result = self.call(Recorder(FakeResponse({"choices": []})), task="t")
        self.assertIn("unexpected response", result["error"])

    def test_ok_call_is_logged(self):
        self.call(Recorder(chat_reply("done")), task="t")
        entry = self.log_lines()[-1]
        self.assertEqual(entry["event"], "ok")
        self.assertEqual(entry["output"], "done")
        self.assertIn("ts", entry)


class McpRegistrationTests(unittest.TestCase):
    """The tool must exist on local runs only (the user-facing contract)."""

    SCRIPT = (
        "import asyncio, mcp_server\n"
        "tools = asyncio.run(mcp_server.mcp.list_tools())\n"
        "print(len(tools), any(t.name == 'delegate_to_small_model' for t in tools))\n"
    )

    def _tool_summary(self, enable):
        env = dict(os.environ)
        env.pop("GENGIN_SMALL_MODEL_ENABLE", None)
        if enable:
            env["GENGIN_SMALL_MODEL_ENABLE"] = "1"
        proc = subprocess.run([sys.executable, "-c", self.SCRIPT],
                              cwd=_LLMOPT_DIR, env=env, capture_output=True,
                              text=True, timeout=120)
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        last = [ln for ln in proc.stdout.splitlines() if ln.strip()][-1]
        count, found = last.split()
        return int(count), found == "True"

    def test_local_run_registers_the_tool(self):
        _, found = self._tool_summary(enable=True)
        self.assertTrue(found)

    def test_cloud_run_does_not_register_the_tool(self):
        _, found = self._tool_summary(enable=False)
        self.assertFalse(found)


if __name__ == "__main__":
    unittest.main()
