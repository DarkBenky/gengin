#!/usr/bin/env python3
"""Unit tests for llmOpt/quick_ask.py (Jev decisions-only).

Everything is mocked at the urllib boundary (no network, no API key), plus a
set of live end-to-end tests against a real local HTTP server.
Run with: python3 -m unittest discover -s llmOpt/tests -v
"""

import inspect
import io
import json
import os
import sys
import tempfile
import time
import unittest
import urllib.error
from http.server import BaseHTTPRequestHandler
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import quick_ask as qa  # noqa: E402

MODULE_DEFAULTS = (
    "MODEL", "DECISIONS_URL", "TIMEOUT_SECONDS", "MAX_CONTEXT_CHARS",
    "MAX_QUESTION_CHARS", "MAX_QUESTIONS", "LOG_PATH",
)


class FakeResponse:
    def __init__(self, body, delay=0.0):
        self._body = (body if isinstance(body, bytes)
                      else json.dumps(body).encode("utf-8"))
        self._delay = delay

    def read(self, size=-1):
        if self._delay:
            time.sleep(self._delay)
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


def http_error(code, body=b"{}"):
    return urllib.error.HTTPError(
        "https://openrouter.ai/x", code, "error", {}, io.BytesIO(body))


def decisions_reply(answers, usage=None):
    return FakeResponse({
        "id": "gen-dec-test",
        "model": "typesafe/jev-1.13-20260917",
        "provider": "TypeSafe",
        "answers": answers,
        "usage": usage or {"input_tokens": 100, "output_tokens": 20,
                           "cost": 0.00001},
    })


def noul(value):
    return {"type": "noul", "noul": value}


class QuickAskTestBase(unittest.TestCase):
    def setUp(self):
        self._saved = {name: getattr(qa, name) for name in MODULE_DEFAULTS}
        self.addCleanup(self._restore)
        self._saved_session = os.environ.pop("GENGIN_SESSION_ID", None)
        self.addCleanup(self._restore_session)
        self._env_patch = mock.patch.dict(
            os.environ, {"OPENROUTER_API_KEY": "test-key"}, clear=False)
        self._env_patch.start()
        self.addCleanup(self._env_patch.stop)
        self.tmpdir = tempfile.mkdtemp(prefix="quick-ask-test-")
        qa.MODEL = "typesafe/jev-1.13"
        qa.DECISIONS_URL = "https://openrouter.ai/api/alpha/decisions"
        qa.TIMEOUT_SECONDS = 5.0
        qa.MAX_CONTEXT_CHARS = 24000
        qa.MAX_QUESTION_CHARS = 8000
        qa.MAX_QUESTIONS = 24
        qa.LOG_PATH = os.path.join(self.tmpdir, "quick_ask.jsonl")
        qa._cache.clear()

    def _restore(self):
        for name, value in self._saved.items():
            setattr(qa, name, value)
        qa._cache.clear()

    def _restore_session(self):
        if self._saved_session is not None:
            os.environ["GENGIN_SESSION_ID"] = self._saved_session

    def call(self, recorder, **kwargs):
        with mock.patch("urllib.request.urlopen", recorder):
            return qa.quick_ask(**kwargs)

    def log_lines(self):
        if not os.path.exists(qa.LOG_PATH):
            return []
        with open(qa.LOG_PATH, encoding="utf-8") as fh:
            return [json.loads(line) for line in fh if line.strip()]


class ChoiceModeTests(QuickAskTestBase):
    def test_choices_success_full(self):
        recorder = Recorder(decisions_reply({
            "answer": {"type": "choice", "choice": "b", "confidence": 0.7,
                       "probabilities": {"a": 0.3, "b": 0.7}},
        }))
        result = self.call(recorder, question="Which?", context="some ctx",
                           choices=["a", "b"])
        self.assertEqual(result, {"choice": "b", "confidence": 0.7,
                                  "probabilities": {"a": 0.3, "b": 0.7}})
        self.assertEqual(recorder.urls[0], qa.DECISIONS_URL)
        body = recorder.bodies[0]
        self.assertEqual(body["model"], "typesafe/jev-1.13")
        self.assertEqual(body["state"], {"context": "some ctx"})
        question = body["questions"]["answer"]
        self.assertEqual(question["type"], "choice")
        self.assertEqual(question["instructions"], "Which?")
        self.assertEqual(question["criteria"], {"a": "a", "b": "b"})
        # no chat-style fields ever appear on this path
        for forbidden in ("messages", "response_format", "provider",
                          "temperature", "max_tokens"):
            self.assertNotIn(forbidden, body)

    def test_choices_minimal_answer(self):
        recorder = Recorder(decisions_reply(
            {"answer": {"type": "choice", "choice": "a"}}))
        result = self.call(recorder, question="q", choices=["a", "b"])
        self.assertEqual(result, {"choice": "a"})

    def test_choices_confidence_boundaries(self):
        for value in (0, 0.5, 1):
            qa._cache.clear()  # identical requests would hit the cache
            recorder = Recorder(decisions_reply({
                "answer": {"type": "choice", "choice": "a",
                           "confidence": value}}))
            result = self.call(recorder, question="q", choices=["a", "b"])
            self.assertEqual(result["confidence"], float(value), msg=repr(value))

    def test_choices_confidence_invalid(self):
        for value in (-0.1, 1.5, "high", True):
            recorder = Recorder(decisions_reply({
                "answer": {"type": "choice", "choice": "a",
                           "confidence": value}}))
            result = self.call(recorder, question="q", choices=["a", "b"])
            self.assertIn("error", result, msg=repr(value))

    def test_choices_probabilities_validation(self):
        bad_probabilities = (
            {"zzz": 0.5},          # label not in criteria
            {"a": "high"},         # non-numeric value
            "nope",                # not an object
        )
        for probabilities in bad_probabilities:
            recorder = Recorder(decisions_reply({
                "answer": {"type": "choice", "choice": "a",
                           "probabilities": probabilities}}))
            result = self.call(recorder, question="q", choices=["a", "b"])
            self.assertIn("error", result, msg=repr(probabilities))

    def test_choices_outside_criteria(self):
        recorder = Recorder(decisions_reply({
            "answer": {"type": "choice", "choice": "zzz"}}))
        result = self.call(recorder, question="q", choices=["a", "b"])
        self.assertIn("error", result)
        self.assertIn("not in", result["error"])

    def test_choices_answer_type_mismatch(self):
        recorder = Recorder(decisions_reply({"answer": noul(0.5)}))
        result = self.call(recorder, question="q", choices=["a", "b"])
        self.assertIn("unexpected type", result["error"])

    def test_choices_validation_errors(self):
        for bad in (["a"], ["a", "a"], ["a", ""], ["a", 3], [], "ab"):
            result = qa.quick_ask(question="q", choices=bad)
            self.assertIn("error", result, msg=repr(bad))

    def test_choices_many_labels(self):
        labels = [f"c{i}" for i in range(20)]
        recorder = Recorder(decisions_reply({
            "answer": {"type": "choice", "choice": "c7"}}))
        result = self.call(recorder, question="q", choices=labels)
        self.assertEqual(result, {"choice": "c7"})
        criteria = recorder.bodies[0]["questions"]["answer"]["criteria"]
        self.assertEqual(len(criteria), 20)
        self.assertEqual(criteria["c7"], "c7")

    def test_choices_labels_are_stripped(self):
        recorder = Recorder(decisions_reply({
            "answer": {"type": "choice", "choice": "a"}}))
        result = self.call(recorder, question="q", choices=[" a ", "b"])
        self.assertEqual(result, {"choice": "a"})
        criteria = recorder.bodies[0]["questions"]["answer"]["criteria"]
        self.assertIn("a", criteria)


class YesNoModeTests(QuickAskTestBase):
    def test_yes_no_success(self):
        recorder = Recorder(decisions_reply({"answer": noul(0.96)}))
        result = self.call(recorder, question="Is it?", context="ctx",
                           yes_no=True)
        self.assertEqual(result, {"noul": 0.96})
        question = recorder.bodies[0]["questions"]["answer"]
        self.assertEqual(question["type"], "noul")
        self.assertEqual(question["instructions"], "Is it?")
        self.assertNotIn("criteria", question)

    def test_yes_no_boundaries(self):
        for value in (0, 0.5, 1):
            qa._cache.clear()
            recorder = Recorder(decisions_reply({"answer": noul(value)}))
            result = self.call(recorder, question="q", yes_no=True)
            self.assertEqual(result, {"noul": float(value)}, msg=repr(value))

    def test_yes_no_invalid_values(self):
        for value in (-0.1, 1.2, True, "0.5", None):
            recorder = Recorder(decisions_reply(
                {"answer": {"type": "noul", "noul": value}}))
            result = self.call(recorder, question="q", yes_no=True)
            self.assertIn("error", result, msg=repr(value))
        recorder = Recorder(decisions_reply({"answer": {"type": "noul"}}))
        result = self.call(recorder, question="q", yes_no=True)
        self.assertIn("error", result)

    def test_yes_no_answer_mismatch(self):
        cases = (
            {},                                                    # missing
            {"answer": noul(0.5), "other": noul(0.5)},             # extra
            {"answer": "nope"},                                    # non-dict
            {"answer": {"type": "choice", "choice": "a"}},         # type
        )
        for answers in cases:
            recorder = Recorder(decisions_reply(answers))
            result = self.call(recorder, question="q", yes_no=True)
            self.assertIn("error", result, msg=repr(answers))


class BatteryTests(QuickAskTestBase):
    QUESTIONS = {
        "relevant": {"type": "noul", "instructions": "Is it relevant?"},
        "kind": {"type": "choice", "instructions": "What kind?",
                 "criteria": {"idea": "an idea", "bug": "a bug"}},
        "quality": {"type": "score", "instructions": "How good?",
                    "criteria": ["bad", "ok", "great"]},
    }

    def test_battery_success_full(self):
        recorder = Recorder(decisions_reply({
            "relevant": noul(0.9),
            "kind": {"type": "choice", "choice": "idea", "confidence": 0.8,
                     "probabilities": {"idea": 0.8, "bug": 0.2}},
            "quality": {"type": "score", "score": 1.5, "confidence": 0.6,
                        "probabilities": {"0": 0.1, "1": 0.3, "2": 0.6},
                        "legend": {"0": "bad", "1": "ok", "2": "great"}},
        }))
        result = self.call(recorder, context="ctx", questions=self.QUESTIONS)
        self.assertEqual(set(result), {"answers"})
        answers = result["answers"]
        self.assertEqual(answers["relevant"], {"noul": 0.9})
        self.assertEqual(answers["kind"]["choice"], "idea")
        self.assertEqual(answers["kind"]["confidence"], 0.8)
        self.assertEqual(answers["quality"]["score"], 1.5)
        self.assertEqual(answers["quality"]["legend"]["2"], "great")
        body = recorder.bodies[0]
        self.assertEqual(body["state"], {"context": "ctx"})
        self.assertEqual(body["questions"]["relevant"],
                         {"type": "noul", "instructions": "Is it relevant?"})
        self.assertEqual(body["questions"]["kind"]["criteria"],
                         {"idea": "an idea", "bug": "a bug"})
        self.assertEqual(body["questions"]["quality"]["criteria"],
                         ["bad", "ok", "great"])
        line = self.log_lines()[0]
        self.assertEqual(line["mode"], "questions")
        self.assertEqual(line["question_count"], 3)
        self.assertEqual(line["question_types"]["kind"], "choice")

    def test_battery_score_minimal(self):
        recorder = Recorder(decisions_reply({
            "quality": {"type": "score", "score": 2.0}}))
        result = self.call(recorder, context="ctx", questions={
            "quality": {"type": "score", "instructions": "How good?",
                        "criteria": ["bad", "great"]}})
        self.assertEqual(result, {"answers": {"quality": {"score": 2.0}}})

    def test_battery_partial_probabilities_allowed(self):
        recorder = Recorder(decisions_reply({
            "kind": {"type": "choice", "choice": "a",
                     "probabilities": {"a": 1.0}}}))
        result = self.call(recorder, context="ctx", questions={
            "kind": {"type": "choice", "instructions": "k",
                     "criteria": {"a": "a", "b": "b"}}})
        self.assertEqual(result["answers"]["kind"]["probabilities"],
                         {"a": 1.0})

    def test_battery_noul_criteria_allowed(self):
        recorder = Recorder(decisions_reply({"q": noul(0.5)}))
        result = self.call(recorder, context="ctx", questions={
            "q": {"type": "noul", "instructions": "x",
                  "criteria": {"true": "yes", "false": "no"}}})
        self.assertEqual(result, {"answers": {"q": {"noul": 0.5}}})
        sent = recorder.bodies[0]["questions"]["q"]
        self.assertEqual(sent["criteria"], {"true": "yes", "false": "no"})

    def test_battery_ten_questions(self):
        questions = {f"q{i}": {"type": "noul", "instructions": f"Is {i}?"}
                     for i in range(10)}
        answers = {f"q{i}": noul(i / 10.0) for i in range(10)}
        recorder = Recorder(decisions_reply(answers))
        result = self.call(recorder, context="ctx", questions=questions)
        self.assertEqual(len(result["answers"]), 10)
        self.assertEqual(result["answers"]["q5"]["noul"], 0.5)
        self.assertEqual(len(recorder.bodies[0]["questions"]), 10)

    def test_battery_deepcopy_cache_isolation(self):
        questions = {"kind": {"type": "choice", "instructions": "k",
                              "criteria": {"a": "a", "b": "b"}}}
        first = Recorder(decisions_reply({
            "kind": {"type": "choice", "choice": "a",
                     "probabilities": {"a": 1.0, "b": 0.0}}}))
        result1 = self.call(first, context="ctx", questions=questions)
        result1["answers"]["kind"]["probabilities"]["a"] = 0.123
        second = Recorder()
        result2 = self.call(second, context="ctx", questions=questions)
        self.assertEqual(second.requests, [])
        self.assertEqual(result2["answers"]["kind"]["probabilities"]["a"], 1.0)

    def test_battery_spec_validation(self):
        bad_specs = (
            "not a dict",
            {},
            {"q": "not an object"},
            {"q": {"type": "magic", "instructions": "x"}},
            {"q": {"type": "noul"}},
            {"q": {"type": "noul", "instructions": "   "}},
            {"q": {"type": "choice", "instructions": "x",
                   "criteria": {"only": "one"}}},
            {"q": {"type": "choice", "instructions": "x"}},
            {"q": {"type": "score", "instructions": "x", "criteria": []}},
            {"q": {"type": "score", "instructions": "x"}},
            {"q": {"type": "noul", "instructions": "x",
                   "criteria": {"true": "yes"}}},
            {"q": {"type": "noul", "instructions": "x", "criteria": "nope"}},
            {" ": {"type": "noul", "instructions": "x"}},
        )
        for spec in bad_specs:
            result = qa.quick_ask(context="ctx", questions=spec)
            self.assertIn("error", result, msg=repr(spec))

    def test_battery_limits(self):
        qa.MAX_QUESTIONS = 1
        result = qa.quick_ask(context="ctx", questions=self.QUESTIONS)
        self.assertIn("at most 1", result["error"])
        qa.MAX_QUESTIONS = 24
        qa.MAX_QUESTION_CHARS = 10
        result = qa.quick_ask(context="ctx", questions={
            "q": {"type": "noul", "instructions": "x" * 11}})
        self.assertIn("exceed", result["error"])
        recorder = Recorder(decisions_reply({"q": noul(0.5)}))
        result = self.call(recorder, context="ctx", questions={
            "q": {"type": "noul", "instructions": "x" * 10}})
        self.assertEqual(result, {"answers": {"q": {"noul": 0.5}}})

    def test_battery_question_and_questions_rejected(self):
        result = qa.quick_ask(question="why", questions=self.QUESTIONS)
        self.assertIn("not both", result["error"])

    def test_battery_answer_validation(self):
        questions = {
            "kind": {"type": "choice", "instructions": "k",
                     "criteria": {"a": "a", "b": "b"}},
            "quality": {"type": "score", "instructions": "s",
                        "criteria": ["bad", "great"]},
        }
        bad_answers = (
            {"kind": {"type": "choice", "choice": "zzz"},
             "quality": {"type": "score", "score": 1}},
            {"kind": {"type": "choice", "choice": "a"},
             "quality": {"type": "score", "score": "high"}},
            {"kind": {"type": "choice", "choice": "a"},
             "quality": {"type": "noul", "noul": 0.5}},
            {"kind": "nope", "quality": {"type": "score", "score": 1}},
        )
        for answers in bad_answers:
            recorder = Recorder(decisions_reply(answers))
            result = self.call(recorder, context="ctx", questions=questions)
            self.assertIn("error", result, msg=repr(answers))


class ValidationTests(QuickAskTestBase):
    def test_chat_surface_removed(self):
        params = set(inspect.signature(qa.quick_ask).parameters)
        self.assertEqual(
            params, {"question", "context", "choices", "yes_no", "questions"})
        for removed in ("BACKEND", "BACKENDS", "CHAT_BASE_URL", "CHAT_MODEL",
                        "QUANTIZATIONS", "MAX_FREE_TOKENS"):
            self.assertFalse(hasattr(qa, removed), removed)

    def test_no_mode_rejected(self):
        result = qa.quick_ask(question="q")
        self.assertIn("pass one of", result["error"])
        self.assertIn("never generates text", result["error"])
        result = qa.quick_ask()
        self.assertIn("pass one of", result["error"])

    def test_multiple_modes_rejected(self):
        combos = (
            {"choices": ["a", "b"], "yes_no": True},
            {"choices": ["a", "b"],
             "questions": {"q": {"type": "noul", "instructions": "x"}}},
            {"yes_no": True,
             "questions": {"q": {"type": "noul", "instructions": "x"}}},
        )
        for combo in combos:
            result = qa.quick_ask(question="q", **combo)
            self.assertIn("exactly one", result["error"], msg=repr(combo))

    def test_question_validation(self):
        for bad in ("", "   ", None):
            result = qa.quick_ask(question=bad, yes_no=True)
            self.assertIn("non-empty", result["error"], msg=repr(bad))

    def test_question_at_limit_is_allowed(self):
        qa.MAX_QUESTION_CHARS = 10
        recorder = Recorder(decisions_reply({"answer": noul(0.5)}))
        result = self.call(recorder, question="x" * 10, yes_no=True)
        self.assertEqual(result, {"noul": 0.5})
        result = qa.quick_ask(question="x" * 11, yes_no=True)
        self.assertIn("exceeds", result["error"])

    def test_context_non_string_rejected(self):
        for bad in (0, False, ["x"]):
            result = qa.quick_ask(question="q", yes_no=True, context=bad)
            self.assertIn("context", result["error"], msg=repr(bad))

    def test_context_truncation_and_log(self):
        qa.MAX_CONTEXT_CHARS = 10
        recorder = Recorder(decisions_reply({"answer": noul(0.5)}))
        result = self.call(recorder, question="q", context="C" * 50,
                           yes_no=True)
        self.assertEqual(result, {"noul": 0.5})
        sent = recorder.bodies[0]["state"]["context"]
        self.assertEqual(sent, "C" * 10)
        line = self.log_lines()[0]
        self.assertTrue(line["context_truncated"])
        self.assertEqual(line["context_chars"], 10)

    def test_decisions_url_must_be_http(self):
        qa.DECISIONS_URL = "ftp://example.com/decisions"
        result = qa.quick_ask(question="q", yes_no=True)
        self.assertIn("QUICK_ASK_DECISIONS_URL", result["error"])

    def test_model_override_used(self):
        qa.MODEL = "typesafe/jev-test"
        recorder = Recorder(decisions_reply({"answer": noul(0.5)}))
        self.call(recorder, question="q", yes_no=True)
        self.assertEqual(recorder.bodies[0]["model"], "typesafe/jev-test")

    def test_session_id_passed_and_truncated(self):
        os.environ["GENGIN_SESSION_ID"] = "sess-1"
        recorder = Recorder(decisions_reply({"answer": noul(0.5)}))
        self.call(recorder, question="q", yes_no=True)
        self.assertEqual(recorder.bodies[0]["session_id"], "sess-1")
        os.environ["GENGIN_SESSION_ID"] = "s" * 300
        recorder = Recorder(decisions_reply({"answer": noul(0.5)}))
        self.call(recorder, question="other", yes_no=True)
        self.assertEqual(len(recorder.bodies[0]["session_id"]), 256)

    def test_session_id_omitted_when_unset(self):
        recorder = Recorder(decisions_reply({"answer": noul(0.5)}))
        self.call(recorder, question="q", yes_no=True)
        self.assertNotIn("session_id", recorder.bodies[0])


class ErrorHandlingTests(QuickAskTestBase):
    def test_transient_codes(self):
        for code in (429, 500, 502, 503, 524, 529):
            recorder = Recorder(http_error(code, b'{"error": {"message": "x"}}'))
            result = self.call(recorder, question="q", yes_no=True)
            self.assertIn(str(code), result["error"], msg=code)
            self.assertIn("transient", result["error"], msg=code)

    def test_terminal_codes(self):
        for code in (400, 401, 403, 404, 413):
            recorder = Recorder(http_error(code, b'{"error": {"message": "x"}}'))
            result = self.call(recorder, question="q", yes_no=True)
            self.assertIn(str(code), result["error"], msg=code)
            self.assertNotIn("transient", result["error"], msg=code)

    def test_402_terminal_vs_transient(self):
        recorder = Recorder(http_error(
            402, b'{"error": {"message": "Insufficient credits"}}'))
        result = self.call(recorder, question="q", yes_no=True)
        self.assertIn("terminal", result["error"])
        body = json.dumps({"error": {
            "code": 402, "message": "busy",
            "metadata": {"limit_source": "openrouter_in_flight_budget"}},
        }).encode("utf-8")
        recorder = Recorder(http_error(402, body))
        result = self.call(recorder, question="q", yes_no=True)
        self.assertIn("transient", result["error"])

    def test_connection_error(self):
        recorder = Recorder(urllib.error.URLError("connection refused"))
        result = self.call(recorder, question="q", yes_no=True)
        self.assertIn("request failed", result["error"])

    def test_non_json_body(self):
        recorder = Recorder(FakeResponse(b"not json"))
        result = self.call(recorder, question="q", yes_no=True)
        self.assertIn("non-JSON", result["error"])

    def test_malformed_answers_never_raise(self):
        payloads = (
            {},
            {"answers": None},
            {"answers": "nope"},
            {"answers": {"answer": "nope"}},
            {"answers": {"answer": {}}},
            {"answers": {"answer": {"type": "noul"}}},
        )
        for payload in payloads:
            recorder = Recorder(FakeResponse(payload))
            result = self.call(recorder, question="q", yes_no=True)
            self.assertIn("error", result, msg=repr(payload))

    def test_timeout_is_a_wall_clock_deadline(self):
        qa.TIMEOUT_SECONDS = 0.2
        recorder = Recorder(FakeResponse({"never": "read"}, delay=2.0))
        started = time.monotonic()
        result = self.call(recorder, question="q", yes_no=True)
        elapsed = time.monotonic() - started
        self.assertIn("error", result)
        self.assertIn("deadline", result["error"])
        self.assertLess(elapsed, 1.2)

    def test_missing_api_key(self):
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("OPENROUTER_API_KEY", None)
            result = qa.quick_ask(question="q", yes_no=True)
        self.assertIn("OPENROUTER_API_KEY", result["error"])

    def test_error_is_logged(self):
        recorder = Recorder(http_error(500, b'{"error": {"message": "x"}}'))
        self.call(recorder, question="q", yes_no=True)
        line = self.log_lines()[0]
        self.assertIn("error", line)
        self.assertIn("500", line["error"])
        self.assertGreaterEqual(line["ms"], 0)

    def test_parse_failure_logs_raw(self):
        recorder = Recorder(decisions_reply({"answer": noul(1.5)}))
        self.call(recorder, question="q", yes_no=True)
        line = self.log_lines()[0]
        self.assertIn("raw", line)
        self.assertIn("noul", line["raw"])


class CacheAndAuditTests(QuickAskTestBase):
    def test_cache_hit_and_logging(self):
        first = Recorder(decisions_reply({"answer": noul(0.9)}))
        result1 = self.call(first, question="same", context="ctx", yes_no=True)
        second = Recorder()  # no scripted responses: any network call fails
        result2 = self.call(second, question="same", context="ctx", yes_no=True)
        self.assertEqual(second.requests, [])
        self.assertEqual(result2.get("cached"), True)
        self.assertEqual({k: v for k, v in result2.items() if k != "cached"},
                         result1)
        lines = self.log_lines()
        self.assertEqual(len(lines), 2)
        self.assertFalse(lines[0].get("cached"))
        self.assertTrue(lines[1]["cached"])
        self.assertEqual(lines[0]["hash"], lines[1]["hash"])
        self.assertEqual(lines[1]["ms"], 0)

    def test_battery_cache_hit_logged(self):
        questions = {"q": {"type": "noul", "instructions": "x"}}
        first = Recorder(decisions_reply({"q": noul(0.4)}))
        self.call(first, context="ctx", questions=questions)
        second = Recorder()
        result = self.call(second, context="ctx", questions=questions)
        self.assertEqual(second.requests, [])
        self.assertEqual(result["cached"], True)
        line = self.log_lines()[1]
        self.assertTrue(line["cached"])
        self.assertEqual(line["mode"], "questions")

    def test_errors_are_not_cached(self):
        failing = Recorder(http_error(500, b'{"error": {"message": "boom"}}'))
        result = self.call(failing, question="q", context="ctx", yes_no=True)
        self.assertIn("error", result)
        succeeding = Recorder(decisions_reply({"answer": noul(0.5)}))
        result = self.call(succeeding, question="q", context="ctx", yes_no=True)
        self.assertEqual(result, {"noul": 0.5})
        self.assertEqual(len(succeeding.requests), 1)

    def test_cache_key_includes_choices(self):
        first = Recorder(decisions_reply({
            "answer": {"type": "choice", "choice": "a"}}))
        result1 = self.call(first, question="pick", choices=["a", "b"])
        second = Recorder(decisions_reply({
            "answer": {"type": "choice", "choice": "x"}}))
        result2 = self.call(second, question="pick", choices=["x", "y"])
        self.assertEqual(len(second.requests), 1)
        self.assertEqual(result1, {"choice": "a"})
        self.assertEqual(result2, {"choice": "x"})

    def test_usage_and_served_model_logged(self):
        recorder = Recorder(decisions_reply({"answer": noul(0.5)}))
        self.call(recorder, question="q", context="ctx", yes_no=True)
        line = self.log_lines()[0]
        self.assertEqual(line["mode"], "yes_no")
        self.assertEqual(line["question"], "q")
        self.assertEqual(line["usage"]["input_tokens"], 100)
        self.assertEqual(line["usage"]["output_tokens"], 20)
        self.assertEqual(line["usage"]["cost"], 0.00001)
        self.assertEqual(line["served_model"], "typesafe/jev-1.13-20260917")
        self.assertTrue(line["ts"].endswith("Z"))

    def test_validation_failures_are_not_logged(self):
        result = qa.quick_ask(question="", yes_no=True)
        self.assertIn("error", result)
        self.assertEqual(self.log_lines(), [])

    def test_log_failure_never_breaks_the_tool(self):
        qa.LOG_PATH = "/dev/null/quick_ask.jsonl"
        recorder = Recorder(decisions_reply({"answer": noul(0.5)}))
        result = self.call(recorder, question="q", yes_no=True)
        self.assertEqual(result, {"noul": 0.5})


class LiveHttpSmokeTests(QuickAskTestBase):
    """End-to-end through a real local HTTP server (no urlopen mocking)."""

    def _serve(self, handler_cls):
        import threading
        from http.server import ThreadingHTTPServer
        server = ThreadingHTTPServer(("127.0.0.1", 0), handler_cls)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        return server

    def test_choices_end_to_end(self):
        captured = {}

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", 0))
                captured["path"] = self.path
                captured["auth"] = self.headers.get("Authorization")
                captured["body"] = json.loads(self.rfile.read(length))
                data = json.dumps({"answers": {
                    "answer": {"type": "choice", "choice": "b",
                               "confidence": 0.9}}}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *args):
                pass

        server = self._serve(Handler)
        qa.DECISIONS_URL = (
            f"http://127.0.0.1:{server.server_port}/api/alpha/decisions")
        result = qa.quick_ask(question="pick", context="ctx",
                              choices=["a", "b"])
        self.assertEqual(result, {"choice": "b", "confidence": 0.9})
        self.assertEqual(captured["path"], "/api/alpha/decisions")
        self.assertEqual(captured["auth"], "Bearer test-key")
        self.assertEqual(captured["body"]["model"], "typesafe/jev-1.13")
        self.assertEqual(captured["body"]["state"], {"context": "ctx"})

    def test_battery_end_to_end(self):
        captured = {}

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", 0))
                captured["body"] = json.loads(self.rfile.read(length))
                data = json.dumps({"answers": {
                    "relevant": {"type": "noul", "noul": 0.8},
                    "duplicate": {"type": "noul", "noul": 0.1},
                }}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *args):
                pass

        server = self._serve(Handler)
        qa.DECISIONS_URL = (
            f"http://127.0.0.1:{server.server_port}/api/alpha/decisions")
        result = qa.quick_ask(context="idea text", questions={
            "relevant": {"type": "noul", "instructions": "Is it relevant?"},
            "duplicate": {"type": "noul", "instructions": "Is it a duplicate?"},
        })
        self.assertEqual(result["answers"]["relevant"]["noul"], 0.8)
        self.assertEqual(result["answers"]["duplicate"]["noul"], 0.1)
        self.assertEqual(len(captured["body"]["questions"]), 2)

    def test_http_error_end_to_end(self):
        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", 0))
                self.rfile.read(length)
                data = b'{"error": {"message": "nope"}}'
                self.send_response(400)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *args):
                pass

        server = self._serve(Handler)
        qa.DECISIONS_URL = (
            f"http://127.0.0.1:{server.server_port}/api/alpha/decisions")
        result = qa.quick_ask(question="q", yes_no=True)
        self.assertIn("400", result["error"])

    def test_deadline_end_to_end(self):
        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", 0))
                self.rfile.read(length)
                time.sleep(1.0)
                data = json.dumps({"answers": {
                    "answer": {"type": "noul", "noul": 0.5}}}).encode()
                try:
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                except (BrokenPipeError, ConnectionResetError):
                    pass  # the client abandoned the request at its deadline

            def log_message(self, *args):
                pass

        server = self._serve(Handler)
        qa.DECISIONS_URL = (
            f"http://127.0.0.1:{server.server_port}/api/alpha/decisions")
        qa.TIMEOUT_SECONDS = 0.2
        started = time.monotonic()
        result = qa.quick_ask(question="q", yes_no=True)
        elapsed = time.monotonic() - started
        self.assertIn("deadline", result["error"])
        self.assertLess(elapsed, 1.2)


if __name__ == "__main__":
    unittest.main()
