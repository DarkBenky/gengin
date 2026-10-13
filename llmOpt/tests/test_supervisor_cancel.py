#!/usr/bin/env python3
"""Supervisor Ctrl-C semantics: first press cancels the session, second stops.

Drives the real supervisor.run_session() against a dummy agent process (no
network, no OpenRouter, no sandbox): a fake key client, route stubs and a
sleep command stand in for the real pieces.

  1. SIGINT mid-session -> agent process group terminated, summary says
     `interrupted`, flags cleared, supervisor still alive (no shutdown).
  2. second SIGINT -> orderly shutdown (and a press while idle stops too).
  3. agent exiting 0 -> completed session, unchanged path.
"""

import json
import os
import shlex
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from decimal import Decimal
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import openrouter_keys  # noqa: E402
import supervisor  # noqa: E402

TARGET_SHA = "a1b2c3d4e5f60718"
KEY_HASH = "f00dcafe00000000"


class FakeKeyClient:
    """Stand-in for OpenRouterClient: no network, records deletions."""

    def __init__(self, management_key=""):
        self.deleted = []

    def create_key(self, session_id, short_sha, limit_usd, expires_at):
        return "sk-fake-inference-key", KEY_HASH

    def get_key_status(self, inference_key):
        return {"limit_remaining": 100.0, "usage": 0.0}

    def delete_key(self, key_hash):
        self.deleted.append(key_hash)
        return True


def make_config(root):
    return supervisor.Config(
        gengin_repo_url="git@github.com:DarkBenky/gengin.git",
        gengin_inputs_dir=os.path.join(root, "inputs"),
        watch_remote="origin",
        watch_branch="main",
        poll_interval_seconds=1,
        run_on_start=False,
        max_setup_retries=3,
        retry_base_seconds=1,
        retry_max_seconds=2,
        openrouter_model="test/model",
        openrouter_budget_usd=Decimal("5.00"),
        session_timeout_seconds=120,
        budget_poll_seconds=1,
        key_expiry_grace_seconds=60,
        termination_grace_seconds=3,
        gengin_display=":99",
        libgl_always_software="",
        headless_mode="",
        preflight_bench_duration_seconds=2,
        require_perf=False,
        log_retention_days=30,
        state_dir=os.path.join(root, "state"),
        session_log_dir=os.path.join(root, "logs"),
        run_dir=os.path.join(root, "run"),
        agent_user="",
        harness="dsh",
        management_key="management-key",
    )


class CtrlCSemanticsTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="sup-cancel-")
        self.addCleanup(self._tmp.cleanup)
        self.config = make_config(self._tmp.name)
        self.client = FakeKeyClient()
        self._terminate_calls = []
        real_terminate = supervisor._terminate_group

        def spy(pgid, grace_seconds, use_helper=False):
            self._terminate_calls.append((pgid, grace_seconds, use_helper))
            return real_terminate(pgid, grace_seconds, use_helper=use_helper)

        for patcher in (
            mock.patch.object(openrouter_keys, "OpenRouterClient",
                              lambda *a, **k: self.client),
            mock.patch.object(supervisor, "register_route", lambda config: {}),
            mock.patch.object(supervisor, "clear_route", lambda route: None),
            mock.patch.object(supervisor, "_open_pr_block", lambda limit=10: ""),
            mock.patch.object(supervisor, "_safe_fingerprint", lambda: "test-fp"),
            mock.patch.object(supervisor, "_terminate_group", spy),
        ):
            patcher.start()
            self.addCleanup(patcher.stop)

        supervisor._shutdown.clear()
        supervisor._cancel_session.clear()
        supervisor._active_session_id = None
        supervisor._install_signal_handlers()

    def tearDown(self):
        # Safety net: kill any dummy agent a failing test left behind.
        try:
            with open(os.path.join(self.config.state_dir, "supervisor.json")) as fh:
                active = json.load(fh).get("activeSession") or {}
            pgid = active.get("processGroupId")
            if pgid:
                os.killpg(pgid, signal.SIGKILL)
        except (OSError, ValueError, json.JSONDecodeError):
            pass
        supervisor._shutdown.clear()
        supervisor._cancel_session.clear()
        supervisor._active_session_id = None

    @classmethod
    def tearDownClass(cls):
        signal.signal(signal.SIGINT, signal.default_int_handler)
        signal.signal(signal.SIGTERM, signal.SIG_DFL)

    def run_session_async(self, agent_cmd):
        patcher = mock.patch.object(
            supervisor, "_launch_command",
            lambda *a, **k: ["bash", "-c", agent_cmd])
        patcher.start()
        self.addCleanup(patcher.stop)

        done = threading.Event()
        box = {}

        def runner():
            box["rc"] = supervisor.run_session(
                self.config, supervisor.default_state(), TARGET_SHA)
            done.set()

        threading.Thread(target=runner, daemon=True).start()
        deadline = time.time() + 15
        while time.time() < deadline and supervisor._active_session_id is None:
            time.sleep(0.05)
        self.assertIsNotNone(supervisor._active_session_id,
                             "dummy agent never registered")
        return done, box

    def read_summary(self):
        names = [n for n in os.listdir(self.config.session_log_dir)
                 if n.endswith(".json")]
        self.assertEqual(len(names), 1, f"expected one summary, got {names}")
        with open(os.path.join(self.config.session_log_dir, names[0])) as fh:
            return json.load(fh)

    def test_first_ctrl_c_cancels_session_and_keeps_polling(self):
        done, box = self.run_session_async("exec sleep 600")
        os.kill(os.getpid(), signal.SIGINT)

        self.assertTrue(done.wait(20), "run_session did not return after SIGINT")
        self.assertEqual(box["rc"], supervisor.EXIT_RUNTIME)
        self.assertFalse(supervisor._shutdown.is_set(),
                         "first Ctrl-C must not stop the supervisor")
        self.assertFalse(supervisor._cancel_session.is_set(),
                         "cancel flag must be cleared for the next session")
        self.assertIsNone(supervisor._active_session_id)

        summary = self.read_summary()
        self.assertEqual(summary["exitReason"], "interrupted",
                         msg=json.dumps(summary.get("warnings")))
        self.assertEqual(summary["exitCode"], -15)
        self.assertEqual(self.client.deleted, [KEY_HASH])

        self.assertEqual(len(self._terminate_calls), 1)
        pgid = self._terminate_calls[0][0]
        self.assertFalse(self._terminate_calls[0][2],
                         "single-user test must not use the root helper")
        with self.assertRaises(ProcessLookupError):
            os.killpg(pgid, 0)

    def test_second_ctrl_c_stops(self):
        done, _box = self.run_session_async("exec sleep 600")
        os.kill(os.getpid(), signal.SIGINT)
        time.sleep(0.4)
        os.kill(os.getpid(), signal.SIGINT)

        self.assertTrue(done.wait(20), "run_session did not return")
        deadline = time.time() + 5
        while not supervisor._shutdown.is_set() and time.time() < deadline:
            time.sleep(0.02)
        self.assertTrue(supervisor._shutdown.is_set(),
                        "second Ctrl-C must stop the supervisor")

        summary = self.read_summary()
        self.assertIn(summary["exitReason"], ("interrupted", "operator_shutdown"))
        self.assertEqual(self.client.deleted, [KEY_HASH])

    def test_ctrl_c_while_idle_stops(self):
        os.kill(os.getpid(), signal.SIGINT)
        deadline = time.time() + 5
        while not supervisor._shutdown.is_set() and time.time() < deadline:
            time.sleep(0.02)
        self.assertTrue(supervisor._shutdown.is_set())
        self.assertFalse(supervisor._cancel_session.is_set())

    def test_loop_keeps_polling_after_cancel(self):
        """run_loop must keep cycling after a cancelled session (only a second
        Ctrl-C, or one while idle, stops it)."""
        calls = []

        def fake_run_once(config, state):
            calls.append(len(calls) + 1)
            if len(calls) == 1:
                return supervisor.run_session(config, state, TARGET_SHA)
            return supervisor.EXIT_OK

        for patcher in (
            mock.patch.object(supervisor, "run_once", fake_run_once),
            mock.patch.object(supervisor, "startup_recovery", lambda *a: None),
            mock.patch.object(supervisor, "reconcile_keys", lambda *a: None),
            mock.patch.object(supervisor, "prune_logs", lambda *a: None),
            mock.patch.object(supervisor, "_open_pr_block", lambda limit=10: ""),
            mock.patch.object(
                supervisor, "_launch_command",
                lambda *a, **k: ["bash", "-c", "exec sleep 600"]),
        ):
            patcher.start()
            self.addCleanup(patcher.stop)

        done = threading.Event()
        box = {}

        def runner():
            box["rc"] = supervisor.run_loop(
                self.config, supervisor.default_state())
            done.set()

        threading.Thread(target=runner, daemon=True).start()
        deadline = time.time() + 15
        while time.time() < deadline and supervisor._active_session_id is None:
            time.sleep(0.05)
        self.assertIsNotNone(supervisor._active_session_id)

        os.kill(os.getpid(), signal.SIGINT)  # cancel the session
        deadline = time.time() + 20
        while time.time() < deadline and len(calls) < 2:
            time.sleep(0.05)
        self.assertGreaterEqual(len(calls), 2,
                                "loop must keep polling after a cancel")
        self.assertFalse(supervisor._shutdown.is_set())

        os.kill(os.getpid(), signal.SIGINT)  # idle now - stops the loop
        self.assertTrue(done.wait(20), "run_loop did not return")
        self.assertTrue(supervisor._shutdown.is_set())
        self.assertEqual(box["rc"], supervisor.EXIT_OK)

    def test_group_sigint_console_simulation(self):
        """A tmux Ctrl-C hits the whole foreground group (console bash +
        python). The first one must not kill either; the second ends both
        cleanly."""
        llmopt_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        child = os.path.join(self._tmp.name, "console_child.py")
        with open(child, "w") as fh:
            fh.write(
                "import sys, time\n"
                f"sys.path.insert(0, {llmopt_dir!r})\n"
                "import supervisor\n"
                "supervisor._install_signal_handlers()\n"
                "supervisor._active_session_id = 'simulated-session'\n"
                "print('READY', flush=True)\n"
                "deadline = time.time() + 40\n"
                "while not supervisor._shutdown.is_set() and time.time() < deadline:\n"
                "    time.sleep(0.05)\n"
                "print('DONE', flush=True)\n"
                "sys.exit(0 if supervisor._shutdown.is_set() else 3)\n")
        script = (f"set -euo pipefail; {shlex.quote(sys.executable)} "
                  f"{shlex.quote(child)}")
        proc = subprocess.Popen(
            ["bash", "-c", script], start_new_session=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

        def cleanup():
            if proc.poll() is None:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except OSError:
                    pass
        self.addCleanup(cleanup)

        self.assertEqual(proc.stdout.readline().strip(), "READY")
        os.killpg(proc.pid, signal.SIGINT)  # first Ctrl-C: group-wide
        time.sleep(1.2)
        self.assertIsNone(proc.poll(),
                          "first Ctrl-C must not kill the console chain")
        os.killpg(proc.pid, signal.SIGINT)  # second Ctrl-C: stop
        out, _ = proc.communicate(timeout=10)
        self.assertIn("DONE", out)
        self.assertEqual(proc.returncode, 0)

    def test_normal_session_is_unchanged(self):
        done, box = self.run_session_async("exit 0")

        self.assertTrue(done.wait(20))
        self.assertEqual(box["rc"], supervisor.EXIT_OK)
        summary = self.read_summary()
        self.assertEqual(summary["exitReason"], "completed")
        self.assertEqual(summary["exitCode"], 0)
        self.assertFalse(supervisor._shutdown.is_set())
        self.assertFalse(supervisor._cancel_session.is_set())
        self.assertIsNone(supervisor._active_session_id)
        self.assertEqual(self.client.deleted, [KEY_HASH])


if __name__ == "__main__":
    unittest.main()
