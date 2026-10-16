#!/usr/bin/env python3
"""PR Environment section + verification tooling tests (llmOpt/main.py)."""

import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import main  # noqa: E402


class FakeCompleted:
    def __init__(self, returncode=0, stdout="", stderr=""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


SAMPLE_INFO = {
    "machine": "desk-a",
    "cpu": "AMD Ryzen 9 5900X 12-Core Processor",
    "threads": 24,
    "gpus": ["NVIDIA GeForce RTX 3090 (24576 MiB)",
             "NVIDIA GeForce RTX 3060 (12288 MiB)"],
    "opencl": "Device #0: NVIDIA GeForce RTX 3090",
    "compiler": "Ubuntu clang version 18.1.3",
    "kernel": "6.14.0-generic",
}


class MachineInfoTests(unittest.TestCase):
    def test_parses_gpus_opencl_compiler(self):
        def fake_run(cmd, **kwargs):
            if cmd[0] == "nvidia-smi":
                return FakeCompleted(stdout="NVIDIA GeForce RTX 3090, 24576\n"
                                            "NVIDIA GeForce RTX 3060, 12288\n")
            if cmd[0] == "clinfo":
                return FakeCompleted(stdout="Platform #0: NVIDIA CUDA\n"
                                            " `-- Device #0: NVIDIA GeForce RTX 3090\n")
            if cmd[0] == "clang":
                return FakeCompleted(stdout="Ubuntu clang version 18.1.3\nmore\n")
            return FakeCompleted()

        with mock.patch.object(main.subprocess, "run", side_effect=fake_run):
            info = main.machineInfo()
        self.assertEqual(info["gpus"], ["NVIDIA GeForce RTX 3090 (24576 MiB)",
                                        "NVIDIA GeForce RTX 3060 (12288 MiB)"])
        self.assertEqual(info["opencl"], "Device #0: NVIDIA GeForce RTX 3090")
        self.assertEqual(info["compiler"], "Ubuntu clang version 18.1.3")
        self.assertTrue(info["machine"])
        self.assertTrue(info["kernel"])

    def test_missing_probes_never_raise(self):
        with mock.patch.object(main.subprocess, "run", side_effect=OSError("missing")), \
                mock.patch.object(main.os, "listdir", side_effect=OSError("missing")):
            info = main.machineInfo()
        self.assertEqual(info["gpus"], [])
        self.assertEqual(info["opencl"], "unknown")
        self.assertEqual(info["compiler"], "unknown")
        self.assertEqual(info["threads"], os.cpu_count() or 0)

    def test_opencl_falls_back_to_icd_vendors(self):
        with mock.patch.object(main.subprocess, "run", side_effect=OSError("missing")), \
                mock.patch.object(main.os, "listdir",
                                  return_value=["nvidia.icd", "cuda.icd", "notes.txt"]):
            info = main.machineInfo()
        self.assertEqual(info["opencl"], "ICDs: cuda.icd, nvidia.icd")


class EnvironmentSectionTests(unittest.TestCase):
    def test_section_marker_and_rows(self):
        text = main._environmentSectionMarkdown(SAMPLE_INFO)
        self.assertTrue(text.startswith("## Environment\n"))
        self.assertIn("<!-- gengin-machine: desk-a -->", text)
        self.assertIn("| Machine | `desk-a` |", text)
        self.assertIn("| CPU | AMD Ryzen 9 5900X 12-Core Processor (24 threads) |", text)
        self.assertIn("NVIDIA GeForce RTX 3090 (24576 MiB)", text)
        self.assertIn("| Kernel | 6.14.0-generic |", text)

    def test_cpu_only_host(self):
        text = main._environmentSectionMarkdown(dict(SAMPLE_INFO, gpus=[]))
        self.assertIn("| GPU | none (CPU-only host) |", text)

    def test_extract_machine(self):
        self.assertEqual(
            main._extractMachineFromBody("x\n<!-- gengin-machine: vm-01 -->\ny"),
            "vm-01")
        self.assertEqual(main._extractMachineFromBody("no marker here"), "")
        self.assertEqual(main._extractMachineFromBody(None), "")


class ListPullRequestsMachineTests(unittest.TestCase):
    def _list_one(self, pr):
        with mock.patch.object(main, "_githubRepo", return_value=("o", "r")), \
                mock.patch.object(main, "_githubGetJson", side_effect=[[pr], []]):
            return main.listPullRequests(state="open", limit=1)["pullRequests"][0]

    def test_extracts_machine_from_body(self):
        pr = {"number": 7, "title": "t", "html_url": "u", "user": {"login": "x"},
              "head": {"ref": "b"}, "base": {"ref": "main"}, "state": "open",
              "body": "claims\n<!-- gengin-machine: vm-01 -->\n"}
        self.assertEqual(self._list_one(pr)["machine"], "vm-01")

    def test_missing_body_is_empty_machine(self):
        pr = {"number": 7, "title": "t", "html_url": "u", "user": {"login": "x"},
              "head": {"ref": "b"}, "base": {"ref": "main"}, "state": "open"}
        self.assertEqual(self._list_one(pr)["machine"], "")


class FetchPullRequestBodyTests(unittest.TestCase):
    def test_returns_body_and_machine(self):
        head = "a" * 40
        base = "b" * 40

        def fake_run(cmd, **kwargs):
            args = cmd[1:]
            if args[0] == "fetch":
                return FakeCompleted()
            if args[0] == "rev-parse":
                return FakeCompleted(stdout=head + "\n")
            if args[0] == "merge-base":
                return FakeCompleted(stdout=base + "\n")
            if args[0] == "diff" and "--numstat" in args:
                return FakeCompleted(stdout="1\t0\tmain.c\n")
            if args[0] == "diff":
                return FakeCompleted(stdout="DIFFTEXT\n")
            if args[0] == "apply":
                return FakeCompleted()
            raise AssertionError("unexpected git call: %r" % (args,))

        pr = {"title": "t", "labels": [], "mergeable": True,
              "body": "claims here\n<!-- gengin-machine: vm-01 -->\n"}
        with mock.patch.object(main.subprocess, "run", side_effect=fake_run), \
                mock.patch.object(main, "_githubRepo", return_value=("o", "r")), \
                mock.patch.object(main, "_githubGetJson", return_value=pr):
            out = main.fetchPullRequest(3)
        self.assertEqual(out["machine"], "vm-01")
        self.assertIn("claims here", out["body"])


class CommentPullRequestTests(unittest.TestCase):
    def _open_pr(self):
        return {"state": "open", "merged_at": None,
                "html_url": "https://example/pull/7"}

    def test_rejects_bad_input(self):
        with self.assertRaises(ValueError):
            main.commentPullRequest(0, "x")
        with self.assertRaises(ValueError):
            main.commentPullRequest(True, "x")
        with self.assertRaises(ValueError):
            main.commentPullRequest(7, "   ")
        with self.assertRaises(ValueError):
            main.commentPullRequest(7, "x" * 60001)

    def test_refuses_merged_and_closed(self):
        for pr in ({"state": "closed", "merged_at": "2026-01-01T00:00:00Z"},
                   {"state": "closed", "merged_at": None}):
            with mock.patch.object(main, "_githubRepo", return_value=("o", "r")), \
                    mock.patch.object(main, "_githubGetJson", return_value=pr), \
                    mock.patch.object(main, "_githubSendJson") as send:
                with self.assertRaises(RuntimeError):
                    main.commentPullRequest(7, "verified")
                send.assert_not_called()

    def test_posts_comment_only(self):
        with mock.patch.object(main, "_githubRepo", return_value=("o", "r")), \
                mock.patch.object(main, "_githubGetJson",
                                  return_value=self._open_pr()), \
                mock.patch.object(main, "_githubSendJson", return_value={}) as send:
            out = main.commentPullRequest(7, "Verified on desk-a at abc123: +0.2%")
        self.assertTrue(out["commentPosted"])
        self.assertEqual(send.call_count, 1)
        call = send.call_args_list[0]
        self.assertIn("/issues/7/comments", call.args[0])
        self.assertEqual(call.args[1],
                         {"body": "Verified on desk-a at abc123: +0.2%"})
        self.assertNotIn("method", call.kwargs)


if __name__ == "__main__":
    unittest.main()
