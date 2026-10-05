#!/usr/bin/env python3
"""Supervisor QUICK_ASK_* config and plumbing tests (llmOpt/supervisor.py)."""

import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import supervisor  # noqa: E402

BASE_ENV = """\
GENGIN_REPO_URL=git@github.com:DarkBenky/gengin.git
GENGIN_INPUTS_DIR=/var/lib/gengin-llmopt/inputs
WATCH_REMOTE=origin
WATCH_BRANCH=main
OPENROUTER_MODEL=deepseek/deepseek-v4-flash-0731
OPENROUTER_BUDGET_USD=5.00
GENGIN_DISPLAY=:99
HEADLESS_MODE=xvfb
STATE_DIR=llmOpt/state
SESSION_LOG_DIR=llmOpt/logs/sessions
RUN_DIR=llmOpt/run
POLL_INTERVAL_SECONDS=300
RUN_ON_START=false
MAX_SETUP_RETRIES=3
RETRY_BASE_SECONDS=30
RETRY_MAX_SECONDS=1800
SESSION_TIMEOUT_SECONDS=14400
BUDGET_POLL_SECONDS=15
KEY_EXPIRY_GRACE_SECONDS=900
TERMINATION_GRACE_SECONDS=30
PREFLIGHT_BENCH_DURATION_SECONDS=2
REQUIRE_PERF=true
LOG_RETENTION_DAYS=30
"""

QUICK_ASK_ENV = """\
QUICK_ASK_MODEL=typesafe/jev-1.13
QUICK_ASK_DECISIONS_URL=https://openrouter.ai/api/alpha/decisions
QUICK_ASK_TIMEOUT_SECONDS=45
QUICK_ASK_MAX_CONTEXT_CHARS=30000
QUICK_ASK_MAX_QUESTION_CHARS=9000
QUICK_ASK_MAX_QUESTIONS=10
QUICK_ASK_LOG=llmOpt/logs/quick_ask.jsonl
"""

REMOVED_CHAT_KEYS = (
    "QUICK_ASK_BACKEND=chat",
    "QUICK_ASK_BASE_URL=http://127.0.0.1:8787/api/v1",
    "QUICK_ASK_MAX_TOKENS=150",
    "QUICK_ASK_QUANTIZATIONS=fp8,fp16",
)


class SupervisorQuickAskTests(unittest.TestCase):
    def load(self, extra=""):
        handle = tempfile.NamedTemporaryFile("w", suffix=".env", delete=False)
        handle.write(BASE_ENV + "\n" + extra)
        handle.close()
        self.addCleanup(os.unlink, handle.name)
        with mock.patch.object(supervisor, "ENV_FILE", handle.name):
            return supervisor.load_config(require_management_key=False)

    def test_all_quick_ask_keys_accepted(self):
        config, errors = self.load(QUICK_ASK_ENV)
        self.assertEqual(errors, [])
        self.assertIsNotNone(config)
        self.assertEqual(config.quick_ask_model, "typesafe/jev-1.13")
        self.assertEqual(config.quick_ask_decisions_url,
                         "https://openrouter.ai/api/alpha/decisions")
        self.assertEqual(config.quick_ask_timeout_seconds, 45)
        self.assertEqual(config.quick_ask_max_context_chars, 30000)
        self.assertEqual(config.quick_ask_max_question_chars, 9000)
        self.assertEqual(config.quick_ask_max_questions, 10)
        self.assertTrue(os.path.isabs(config.quick_ask_log))
        self.assertTrue(config.quick_ask_log.endswith(
            os.path.join("llmOpt", "logs", "quick_ask.jsonl")))

    def test_removed_chat_keys_rejected(self):
        for line in REMOVED_CHAT_KEYS:
            key = line.split("=", 1)[0]
            config, errors = self.load(line + "\n")
            self.assertIsNone(config, msg=key)
            self.assertTrue(
                any(key in error for error in errors), msg=key)

    def test_invalid_model_rejected(self):
        _, errors = self.load("QUICK_ASK_MODEL=jevm\n")
        self.assertTrue(any("QUICK_ASK_MODEL" in error for error in errors))

    def test_invalid_decisions_url_rejected(self):
        _, errors = self.load("QUICK_ASK_DECISIONS_URL=not-a-url\n")
        self.assertTrue(
            any("QUICK_ASK_DECISIONS_URL" in error for error in errors))

    def test_out_of_bounds_int_rejected(self):
        _, errors = self.load("QUICK_ASK_MAX_QUESTIONS=0\n")
        self.assertTrue(
            any("QUICK_ASK_MAX_QUESTIONS" in error for error in errors))

    def test_log_outside_repo_rejected(self):
        _, errors = self.load("QUICK_ASK_LOG=/etc/passwd\n")
        self.assertTrue(any("QUICK_ASK_LOG" in error for error in errors))

    def test_unset_defaults(self):
        config, errors = self.load()
        self.assertEqual(errors, [])
        self.assertEqual(config.quick_ask_model, "")
        self.assertEqual(config.quick_ask_timeout_seconds, 0)
        self.assertEqual(config.quick_ask_log, "")

    def test_session_envs_include_overrides(self):
        config, errors = self.load(QUICK_ASK_ENV)
        self.assertEqual(errors, [])
        mcp_env = supervisor._session_mcp_env(
            config, "0" * 40, "sess", "/tmp/result.json")
        self.assertEqual(mcp_env["QUICK_ASK_MODEL"], "typesafe/jev-1.13")
        self.assertEqual(mcp_env["QUICK_ASK_MAX_QUESTIONS"], "10")
        hermes_env = supervisor._build_session_env(
            config, "sess", "/tmp/hermes", "/tmp/q.md", "/tmp/u.json",
            "test-key", "/tmp/result.json")
        self.assertEqual(hermes_env["QUICK_ASK_MODEL"], "typesafe/jev-1.13")
        self.assertEqual(hermes_env["QUICK_ASK_TIMEOUT_SECONDS"], "45")

    def test_openrouter_key_reaches_mcp_env_block(self):
        config, errors = self.load()
        self.assertEqual(errors, [])
        mcp_env = supervisor._session_mcp_env(
            config, "0" * 40, "sess", "/tmp/result.json")
        self.assertIn("OPENROUTER_API_KEY", mcp_env)
        # The renderer emits `KEY: "${KEY}"` lines; Hermes resolves the value.
        rendered = [f'{key}: "${{{key}}}"' for key in mcp_env]
        self.assertIn('OPENROUTER_API_KEY: "${OPENROUTER_API_KEY}"', rendered)
        hermes_env = supervisor._build_session_env(
            config, "sess", "/tmp/hermes", "/tmp/q.md", "/tmp/u.json",
            "capped-key", "/tmp/result.json")
        self.assertEqual(hermes_env["OPENROUTER_API_KEY"], "capped-key")

    def test_unset_overrides_not_exported(self):
        config, errors = self.load()
        self.assertEqual(errors, [])
        mcp_env = supervisor._session_mcp_env(
            config, "0" * 40, "sess", "/tmp/result.json")
        self.assertNotIn("QUICK_ASK_MODEL", mcp_env)
        self.assertNotIn("QUICK_ASK_DECISIONS_URL", mcp_env)


if __name__ == "__main__":
    unittest.main()
