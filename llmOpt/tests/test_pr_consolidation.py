#!/usr/bin/env python3
"""PR consolidation helper tests (llmOpt/main.py)."""

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


class ClosePullRequestTests(unittest.TestCase):
    def test_rejects_bad_number_and_empty_comment(self):
        with self.assertRaises(ValueError):
            main.closePullRequest(0, "Consolidated into #1")
        with self.assertRaises(ValueError):
            main.closePullRequest(True, "Consolidated into #1")
        with self.assertRaises(ValueError):
            main.closePullRequest(5, "   ")

    def test_refuses_merged_pr(self):
        pr = {"state": "closed", "merged_at": "2026-10-06T22:00:00Z",
              "html_url": "https://example/pull/5"}
        with mock.patch.object(main, "_githubRepo", return_value=("o", "r")), \
                mock.patch.object(main, "_githubGetJson", return_value=pr), \
                mock.patch.object(main, "_githubSendJson") as send:
            with self.assertRaises(RuntimeError) as caught:
                main.closePullRequest(5, "Consolidated into #99")
            self.assertIn("merged", str(caught.exception))
            send.assert_not_called()

    def test_refuses_closed_pr(self):
        pr = {"state": "closed", "merged_at": None,
              "html_url": "https://example/pull/5"}
        with mock.patch.object(main, "_githubRepo", return_value=("o", "r")), \
                mock.patch.object(main, "_githubGetJson", return_value=pr), \
                mock.patch.object(main, "_githubSendJson") as send:
            with self.assertRaises(RuntimeError):
                main.closePullRequest(5, "Consolidated into #99")
            send.assert_not_called()

    def test_closes_open_pr_comment_first(self):
        pr = {"state": "open", "merged_at": None,
              "html_url": "https://example/pull/5"}
        with mock.patch.object(main, "_githubRepo", return_value=("o", "r")), \
                mock.patch.object(main, "_githubGetJson", return_value=pr), \
                mock.patch.object(main, "_githubSendJson", return_value={}) as send:
            report = main.closePullRequest(5, "Consolidated into #99")
        self.assertEqual(report["state"], "closed")
        self.assertIn("https://example/pull/5", report["url"])
        self.assertEqual(send.call_count, 2)
        comment_call, close_call = send.call_args_list
        self.assertIn("/issues/5/comments", comment_call.args[0])
        self.assertEqual(comment_call.args[1], {"body": "Consolidated into #99"})
        self.assertIn("/pulls/5", close_call.args[0])
        self.assertEqual(close_call.kwargs.get("method"), "PATCH")


class FetchPullRequestTests(unittest.TestCase):
    def setUp(self):
        self.head = "a" * 40
        self.base = "b" * 40

    def _patch_run(self, apply_rc=0, fetch_rc=0, mergebase_rc=0):
        calls = []

        def fake_run(cmd, **kwargs):
            args = cmd[1:]
            calls.append((args, kwargs))
            if args[0] == "fetch":
                return FakeCompleted(fetch_rc, stderr="" if fetch_rc == 0 else "boom")
            if args[0] == "rev-parse":
                return FakeCompleted(0, stdout=self.head + "\n")
            if args[0] == "merge-base":
                return FakeCompleted(mergebase_rc, stdout=self.base + "\n")
            if args[0] == "diff" and "--numstat" in args:
                return FakeCompleted(0, stdout="12\t3\tfile.c\n1\t0\tdir/other.c\n")
            if args[0] == "diff":
                return FakeCompleted(0, stdout="DIFFTEXT\n")
            if args[0] == "apply":
                return FakeCompleted(apply_rc)
            raise AssertionError("unexpected git call: %r" % (args,))

        patcher = mock.patch.object(main.subprocess, "run", side_effect=fake_run)
        self.addCleanup(patcher.stop)
        patcher.start()
        repo = mock.patch.object(main, "_githubRepo", return_value=("o", "r"))
        self.addCleanup(repo.stop)
        repo.start()
        meta = mock.patch.object(
            main, "_githubGetJson",
            return_value={"title": "flight: tune walk", "mergeable": True,
                          "labels": [{"name": "Flight Controller"}]})
        self.addCleanup(meta.stop)
        meta.start()
        return calls

    def test_rejects_bad_number(self):
        with self.assertRaises(ValueError):
            main.fetchPullRequest(0)

    def test_clean_apply_report(self):
        calls = self._patch_run(apply_rc=0)
        report = main.fetchPullRequest(42)
        self.assertEqual(report["head"], self.head)
        self.assertEqual(report["mergeBase"], self.base)
        self.assertEqual(report["files"], ["file.c", "dir/other.c"])
        self.assertEqual(report["insertions"], 13)
        self.assertEqual(report["deletions"], 3)
        self.assertTrue(report["appliesCleanly"])
        self.assertEqual(report["labels"], ["Flight Controller"])
        self.assertEqual(report["mergeable"], True)
        apply_calls = [c for c in calls if c[0][0] == "apply"]
        self.assertEqual(len(apply_calls), 1)
        self.assertEqual(apply_calls[0][1].get("input"), "DIFFTEXT\n")

    def test_conflict_reports_not_clean(self):
        calls = self._patch_run(apply_rc=1)
        report = main.fetchPullRequest(42)
        self.assertFalse(report["appliesCleanly"])
        self.assertEqual(len([c for c in calls if c[0][0] == "apply"]), 1)
        self.assertNotIn("appliesWithThreeWay", report)

    def test_fetch_failure_raises(self):
        self._patch_run(fetch_rc=1)
        with self.assertRaises(RuntimeError) as caught:
            main.fetchPullRequest(42)
        self.assertIn("pull/42", str(caught.exception))

    def test_no_merge_base_raises(self):
        self._patch_run(mergebase_rc=1)
        with self.assertRaises(RuntimeError) as caught:
            main.fetchPullRequest(42)
        self.assertIn("merge base", str(caught.exception))


class PrLabelsTests(unittest.TestCase):
    def test_canonical_labels_present(self):
        for name in ("Render Improvements",
                     "Render Improvements [Visual change / Performance]",
                     "Render Improvements Visual [No/Minimal Cost]",
                     "Flight Controller", "General Improvements",
                     "ML Improvements"):
            self.assertIn(name, main.PR_LABELS)


class ListPullRequestLabelsTests(unittest.TestCase):
    def test_labels_mapped_to_names(self):
        pr = {"number": 7, "title": "t", "html_url": "u", "state": "open",
              "user": {"login": "a"}, "head": {"ref": "b"},
              "base": {"ref": "main"},
              "labels": [{"name": "Flight Controller"}, {"name": "x"}]}

        def fake_get(url, timeout=30):
            return [pr] if "/pulls?" in url else []

        with mock.patch.object(main, "_githubRepo", return_value=("o", "r")), \
                mock.patch.object(main, "_githubGetJson", side_effect=fake_get):
            out = main.listPullRequests(state="open", limit=1)
        self.assertEqual(out["pullRequests"][0]["labels"],
                         ["Flight Controller", "x"])


class LabelPullRequestTests(unittest.TestCase):
    def test_rejects_unknown_label_without_network(self):
        with mock.patch.object(main, "_githubGetJson") as get:
            with self.assertRaises(ValueError) as caught:
                main.labelPullRequest(5, ["Not A Label"])
        self.assertIn("Not A Label", str(caught.exception))
        get.assert_not_called()

    def test_rejects_empty_or_string_labels(self):
        with self.assertRaises(ValueError):
            main.labelPullRequest(5, [])
        with self.assertRaises(ValueError):
            main.labelPullRequest(5, "Flight Controller")

    def test_refuses_merged_pr(self):
        pr = {"state": "closed", "merged_at": "2026-10-06T22:00:00Z",
              "html_url": "https://example/pull/5", "labels": []}
        with mock.patch.object(main, "_githubRepo", return_value=("o", "r")), \
                mock.patch.object(main, "_githubGetJson", return_value=pr), \
                mock.patch.object(main, "_githubSendJson") as send:
            with self.assertRaises(RuntimeError) as caught:
                main.labelPullRequest(5, ["Flight Controller"])
        self.assertIn("merged", str(caught.exception))
        send.assert_not_called()

    def test_sets_labels_with_replace_put(self):
        pr = {"state": "open", "merged_at": None,
              "html_url": "https://example/pull/5",
              "labels": [{"name": "old"}]}

        def fake_get(url, timeout=30):
            if "/labels" in url:
                return [{"name": "Flight Controller"}]
            return pr

        with mock.patch.object(main, "_githubRepo", return_value=("o", "r")), \
                mock.patch.object(main, "_githubGetJson", side_effect=fake_get), \
                mock.patch.object(main, "_githubSendJson", return_value={}) as send:
            report = main.labelPullRequest(5, ["Flight Controller"])
        self.assertEqual(report["labels"], ["Flight Controller"])
        self.assertEqual(report["previousLabels"], ["old"])
        puts = [c for c in send.call_args_list
                if c.kwargs.get("method") == "PUT"]
        self.assertEqual(len(puts), 1)
        self.assertIn("/issues/5/labels", puts[0].args[0])
        self.assertEqual(puts[0].args[1], {"labels": ["Flight Controller"]})


class CreatePRLabelTests(unittest.TestCase):
    def test_unknown_label_rejected_before_any_work(self):
        with mock.patch.object(main, "_sandboxHead", return_value="c" * 40):
            with self.assertRaises(ValueError) as caught:
                main.createPR("title", "body", branch="llmopt/2ac04754/x",
                              label="Not A Label")
        self.assertIn("Not A Label", str(caught.exception))

    def test_existing_pr_retry_applies_label(self):
        with mock.patch.object(main, "_sandboxHead", return_value="c" * 40), \
                mock.patch.object(main, "run", return_value=FakeCompleted(0)), \
                mock.patch.object(main.subprocess, "run",
                                  return_value=FakeCompleted(0, stdout="")), \
                mock.patch.object(main, "_branchExists", return_value=False), \
                mock.patch.object(main, "_github_find_pr",
                                  return_value=("https://example/pull/7", 7)), \
                mock.patch.object(main, "_github_ensure_labels") as ensure, \
                mock.patch.object(main, "_github_set_labels") as set_labels:
            url = main.createPR("title", "body", branch="llmopt/2ac04754/x",
                                label="Flight Controller")
        self.assertEqual(url, "https://example/pull/7")
        ensure.assert_called_once_with(["Flight Controller"])
        set_labels.assert_called_once_with(7, ["Flight Controller"])

    def test_label_failure_does_not_lose_pr(self):
        with mock.patch.object(main, "_sandboxHead", return_value="c" * 40), \
                mock.patch.object(main, "run", return_value=FakeCompleted(0)), \
                mock.patch.object(main.subprocess, "run",
                                  return_value=FakeCompleted(0, stdout="")), \
                mock.patch.object(main, "_branchExists", return_value=False), \
                mock.patch.object(main, "_github_find_pr",
                                  return_value=("https://example/pull/7", 7)), \
                mock.patch.object(main, "_github_ensure_labels"), \
                mock.patch.object(main, "_github_set_labels",
                                  side_effect=RuntimeError("403")):
            url = main.createPR("title", "body", branch="llmopt/2ac04754/x",
                                label="Flight Controller")
        self.assertEqual(url, "https://example/pull/7")


if __name__ == "__main__":
    unittest.main()
