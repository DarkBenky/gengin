#!/usr/bin/env python3
"""Context-aware route tests (llmOpt/proxy/openrouter_proxy.py)."""

import json
import os
import sys
import time
import unittest

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
LLMOPT_DIR = os.path.dirname(TESTS_DIR)
sys.path.insert(0, LLMOPT_DIR)
sys.path.insert(0, os.path.join(LLMOPT_DIR, "proxy"))

import openrouter_proxy as proxy  # noqa: E402

PRIMARY = "anthropic/claude-haiku-5.5:anthropic"
FALLBACK = "deepseek/deepseek-v4-flash-0731"


def make_body(text="hello"):
    return {"model": PRIMARY,
            "messages": [{"role": "system", "content": "sys"},
                         {"role": "user", "content": text}]}


class RegistrationTests(unittest.TestCase):
    def tearDown(self):
        proxy._route = None

    def test_register_installs_and_clear_is_routeid_guarded(self):
        status, response = proxy._route_register(
            {"members": [PRIMARY, FALLBACK], "maxContext": 99000, "label": "test"})
        self.assertEqual(status, 200)
        self.assertTrue(response["routeId"])
        self.assertEqual(proxy._route["bases"][0], "anthropic/claude-haiku-5.5")
        self.assertEqual(proxy._route["members"][1]["model"], FALLBACK)

        status, _ = proxy._route_clear({"routeId": "bogus"})
        self.assertEqual(status, 409)
        self.assertIsNotNone(proxy._route)

        status, body = proxy._route_clear({"routeId": response["routeId"]})
        self.assertEqual(status, 200)
        self.assertTrue(body["cleared"])
        self.assertIsNone(proxy._route)

    def test_register_rejects_invalid_payloads(self):
        env_key = os.environ.pop("DEEPSEEK_API_KEY", None)
        try:
            cases = [
                {},
                {"members": [PRIMARY], "maxContext": 99000},
                {"members": [PRIMARY, "bad model!"], "maxContext": 99000},
                {"members": [PRIMARY, PRIMARY], "maxContext": 99000},
                {"members": [PRIMARY, FALLBACK], "maxContext": True},
                {"members": [PRIMARY, FALLBACK], "maxContext": 500},
                {"members": [PRIMARY, FALLBACK], "maxContext": 99000,
                 "ttlSeconds": 10 ** 9},
                {"members": [PRIMARY, {"model": "x", "upstream": "bogus"}],
                 "maxContext": 99000},
                {"members": [PRIMARY, {"model": "deepseek-v4-flash",
                                       "upstream": "deepseek"}],
                 "maxContext": 99000},
            ]
            for payload in cases:
                status, _ = proxy._route_register(payload)
                self.assertEqual(status, 400, payload)
            self.assertIsNone(proxy._route)
        finally:
            if env_key is not None:
                os.environ["DEEPSEEK_API_KEY"] = env_key


class EstimateTests(unittest.TestCase):
    def test_no_history_uses_the_whole_body(self):
        self.assertEqual(proxy._estimate_prompt(None, 30000), 10000)

    def test_growth_uses_last_usage(self):
        state = {"lastPrompt": 1000, "lastChars": 12000}
        self.assertEqual(proxy._estimate_prompt(state, 15000), 2000)

    def test_shrunk_body_is_reestimated(self):
        state = {"lastPrompt": 1000, "lastChars": 12000}
        self.assertEqual(proxy._estimate_prompt(state, 3000), 1000)


class SelectionTests(unittest.TestCase):
    def setUp(self):
        proxy._route = None
        status, _ = proxy._route_register(
            {"members": [PRIMARY, FALLBACK], "maxContext": 1000})
        self.assertEqual(status, 200)

    def tearDown(self):
        proxy._route = None

    def test_unrelated_model_is_left_alone(self):
        body = {"model": "xiaomi/mimo-v2.6-flash"}
        model, info = proxy._route_apply(body, body["model"], 5000, "conv")
        self.assertEqual(model, "xiaomi/mimo-v2.6-flash")
        self.assertIsNone(info)

    def test_switch_at_threshold_then_sticky(self):
        # 600 chars -> ~200 estimated tokens: stays on the primary.
        body = make_body()
        model, info = proxy._route_apply(body, PRIMARY, 600, "conv")
        self.assertEqual((info["member"], model), (0, PRIMARY))
        self.assertFalse(info["switched"])

        # Record a real usage the way the proxy does after a response.
        tap = proxy._ResponseTap(False)
        tap.usage = {"prompt_tokens": 900, "completion_tokens": 10, "cost": 0.001}
        proxy._route_observe("conv", info, tap, 600)
        counters = proxy._route["counters"][0]
        self.assertEqual((counters["requests"], counters["promptTokens"]), (1, 900))

        # +600 chars -> 900 + 200 >= 1000: switches to the fallback.
        body = make_body("hello again")
        model, info = proxy._route_apply(body, PRIMARY, 1200, "conv")
        self.assertEqual((info["member"], model), (1, FALLBACK))
        self.assertTrue(info["switched"])
        self.assertEqual(body["model"], FALLBACK)

        # A later, smaller request stays on the fallback (sticky).
        body = make_body()
        model, info = proxy._route_apply(body, PRIMARY, 200, "conv")
        self.assertEqual(info["member"], 1)
        self.assertFalse(info["switched"])


class TapUsageTests(unittest.TestCase):
    def test_streamed_final_usage_chunk(self):
        tap = proxy._ResponseTap(True)
        tap.feed(b'data: {"choices":[{"delta":{"content":"hi"}}]}\n\n')
        tap.feed(b'data: {"choices":[],"usage":{"prompt_tokens":42}}\n\n')
        tap.finish()
        self.assertEqual(tap.usage["prompt_tokens"], 42)

    def test_buffered_body_usage(self):
        tap = proxy._ResponseTap(False)
        tap.feed(json.dumps({"choices": [{"message": {"content": "hi"}}],
                             "usage": {"prompt_tokens": 7}}).encode())
        tap.finish()
        self.assertEqual(tap.usage["prompt_tokens"], 7)


class SkipAndLifecycleTests(unittest.TestCase):
    def setUp(self):
        proxy._route = None
        status, _ = proxy._route_register(
            {"members": [PRIMARY, FALLBACK], "maxContext": 1000})
        self.assertEqual(status, 200)

    def tearDown(self):
        proxy._route = None

    def test_pinned_request_is_not_routed(self):
        body = make_body("x" * 6000)
        body["provider"] = {"order": ["someprovider"]}
        model, info = proxy._route_apply(body, PRIMARY, len(json.dumps(body)),
                                         "conv-pin")
        self.assertEqual(model, PRIMARY)
        self.assertIsNone(info)
        self.assertEqual(body["model"], PRIMARY)
        self.assertEqual(proxy._route["switches"], 0)

    def test_free_variant_is_not_routed(self):
        body = make_body("x" * 6000)
        model, info = proxy._route_apply(body, PRIMARY + ":free",
                                         len(json.dumps(body)), "conv-free")
        self.assertEqual(model, PRIMARY + ":free")
        self.assertIsNone(info)

    def test_first_request_over_threshold_counts_as_switch(self):
        body = make_body("x" * 6000)  # ~2000 estimated tokens >= 1000
        model, info = proxy._route_apply(body, PRIMARY, 6000, "conv-early")
        self.assertEqual((info["member"], model, info["switched"]),
                         (1, FALLBACK, True))
        self.assertEqual(proxy._route["switches"], 1)
        state = proxy._route["conversations"]["conv-early"]
        self.assertEqual(state["member"], 1)
        self.assertIsNotNone(state.get("switchedAt"))

    def test_expired_route_is_dropped(self):
        proxy._route["expiresAt"] = time.time() - 1
        model, info = proxy._route_apply(make_body(), PRIMARY, 100, "conv-exp")
        self.assertIsNone(info)
        self.assertEqual(model, PRIMARY)
        self.assertIsNone(proxy._route)
        self.assertFalse(proxy._route_status()["active"])

    def test_ttl_zero_never_expires(self):
        status, _ = proxy._route_register(
            {"members": [PRIMARY, FALLBACK], "maxContext": 1000, "ttlSeconds": 0})
        self.assertEqual(status, 200)
        self.assertIsNone(proxy._route["expiresAt"])
        self.assertIsNotNone(proxy._route_active())

    def test_replacement_resets_state_and_invalidates_old_clear(self):
        _, first = proxy._route_register(
            {"members": [PRIMARY, FALLBACK], "maxContext": 1000})
        model, info = proxy._route_apply(make_body(), PRIMARY, 600, "conv-r")
        self.assertEqual(info["member"], 0)
        self.assertEqual(len(proxy._route["conversations"]), 1)

        _, second = proxy._route_register(
            {"members": [PRIMARY, FALLBACK], "maxContext": 2000})
        self.assertEqual(proxy._route["conversations"], {})
        status, _ = proxy._route_clear({"routeId": first["routeId"]})
        self.assertEqual(status, 409)
        status, _ = proxy._route_clear({"routeId": second["routeId"]})
        self.assertEqual(status, 200)


class ObserveRobustnessTests(unittest.TestCase):
    def setUp(self):
        proxy._route = None
        status, _ = proxy._route_register(
            {"members": [PRIMARY, FALLBACK], "maxContext": 1000})
        self.assertEqual(status, 200)

    def tearDown(self):
        proxy._route = None

    def test_missing_usage_leaves_state_alone(self):
        body = make_body()
        _, info = proxy._route_apply(body, PRIMARY, 600, "conv-u")
        tap = proxy._ResponseTap(False)  # aborted/empty response: no usage
        proxy._route_observe("conv-u", info, tap, 600)
        self.assertEqual(proxy._route["counters"][0]["requests"], 0)
        self.assertEqual(proxy._route["conversations"]["conv-u"]["lastPrompt"], 0)

    def test_counters_accumulate_across_observes(self):
        body = make_body()
        _, info = proxy._route_apply(body, PRIMARY, 600, "conv-c")
        for prompt, completion, cost in ((900, 10, 0.01), (50, 5, 0.001)):
            tap = proxy._ResponseTap(False)
            tap.usage = {"prompt_tokens": prompt, "completion_tokens": completion,
                         "cost": cost}
            proxy._route_observe("conv-c", info, tap, 600)
        counters = proxy._route["counters"][0]
        self.assertEqual(counters["requests"], 2)
        self.assertEqual(counters["promptTokens"], 950)
        self.assertAlmostEqual(counters["costUsd"], 0.011)

    def test_streamed_leftover_line_without_newline(self):
        tap = proxy._ResponseTap(True)
        tap.feed(b'data: {"choices":[],"usage":{"prompt_tokens":9}}')
        tap.finish()
        self.assertEqual(tap.usage["prompt_tokens"], 9)


class DirectMemberTests(unittest.TestCase):
    def setUp(self):
        proxy._route = None

    def tearDown(self):
        proxy._route = None

    def test_direct_member_selection_rewrites_model(self):
        status, _ = proxy._route_register({
            "members": [PRIMARY, {"model": "deepseek-v4-flash",
                                  "upstream": "deepseek"}],
            "maxContext": 1000, "deepseekKey": "sk-test"})
        self.assertEqual(status, 200)
        body = make_body("x" * 6000)  # ~2000 estimated tokens >= 1000
        model, info = proxy._route_apply(body, PRIMARY, 6000, "conv-direct")
        self.assertEqual(model, "deepseek-v4-flash")
        self.assertEqual(info["upstream"], "deepseek")
        self.assertEqual(body["model"], "deepseek-v4-flash")

    def test_direct_leg_keeps_thinking_and_skips_or_filters(self):
        body = {"model": "deepseek-v4-flash", "stream": True,
                "messages": [{"role": "user", "content": "hi"}]}
        applied, skip = proxy._apply_filters(body, "deepseek-v4-flash",
                                             upstream="deepseek")
        self.assertEqual(applied, ["stream_options"])
        self.assertEqual(skip, "")
        self.assertNotIn("provider", body)
        self.assertNotIn("thinking", body)
        self.assertEqual(body["stream_options"], {"include_usage": True})

    def test_direct_leg_leaves_caller_thinking_alone(self):
        body = {"model": "deepseek-v4-flash", "reasoning_effort": "high",
                "thinking": {"type": "enabled"},
                "messages": [{"role": "user", "content": "hi"}]}
        applied, _ = proxy._apply_filters(body, "deepseek-v4-flash",
                                          upstream="deepseek")
        self.assertEqual(applied, [])
        self.assertEqual(body["thinking"], {"type": "enabled"})
        self.assertEqual(body["reasoning_effort"], "high")

    def test_openrouter_leg_keeps_its_filters(self):
        body = {"model": PRIMARY,
                "messages": [{"role": "user", "content": "hi"}]}
        applied, _ = proxy._apply_filters(body, PRIMARY)
        self.assertIn("pin:anthropic", applied)
        self.assertIn("provider", body)

    def test_upstream_path_translation(self):
        self.assertEqual(
            proxy._upstream_path("/api/v1/chat/completions", "deepseek"),
            "/v1/chat/completions")
        self.assertEqual(
            proxy._upstream_path("/chat/completions", "deepseek"),
            "/chat/completions")
        self.assertEqual(
            proxy._upstream_path("/api/v1/chat/completions", "openrouter"),
            "/api/v1/chat/completions")

    def test_env_key_allows_keyless_registration(self):
        os.environ["DEEPSEEK_API_KEY"] = "sk-env"
        try:
            status, _ = proxy._route_register({
                "members": [PRIMARY, {"model": "deepseek-v4-flash",
                                      "upstream": "deepseek"}],
                "maxContext": 1000})
        finally:
            os.environ.pop("DEEPSEEK_API_KEY", None)
        self.assertEqual(status, 200)


class ReasoningReplayTests(unittest.TestCase):
    def setUp(self):
        with proxy._lock:
            proxy._reasoning_cache.clear()

    def tearDown(self):
        with proxy._lock:
            proxy._reasoning_cache.clear()

    def test_signature_uses_tool_call_ids(self):
        first = {"role": "assistant", "content": None, "tool_calls": [
            {"id": "call_a", "function": {"name": "f", "arguments": "{}"}}]}
        again = {"role": "assistant", "content": None, "tool_calls": [
            {"id": "call_a", "function": {"name": "f", "arguments": " {} "}}]}
        other = {"role": "assistant", "content": None, "tool_calls": [
            {"id": "call_b", "function": {"name": "f", "arguments": "{}"}}]}
        self.assertTrue(proxy._reasoning_signature(first))
        self.assertEqual(proxy._reasoning_signature(first),
                         proxy._reasoning_signature(again))
        self.assertNotEqual(proxy._reasoning_signature(first),
                            proxy._reasoning_signature(other))

    def test_signature_falls_back_to_content(self):
        message = {"role": "assistant", "content": "hello"}
        self.assertTrue(proxy._reasoning_signature(message).startswith("ct:"))
        self.assertNotEqual(proxy._reasoning_signature(message),
                            proxy._reasoning_signature(
                                {"role": "assistant", "content": "other"}))
        self.assertEqual(proxy._reasoning_signature(
            {"role": "assistant", "content": None}), "")

    def test_replay_injects_cached_and_backfills_unknown(self):
        served = {"role": "assistant", "content": "done", "tool_calls": [
            {"id": "call_1", "function": {"name": "f", "arguments": "{}"}}]}
        proxy._reasoning_remember("conv-r", served, "the chain of thought")
        body = {"messages": [
            {"role": "user", "content": "go"},
            {"role": "assistant", "content": "done", "tool_calls": [
                {"id": "call_1", "function": {"name": "f",
                                                 "arguments": "{}"}}]},
            {"role": "tool", "tool_call_id": "call_1", "content": "ok"},
            {"role": "assistant", "content": "never seen"},
            {"role": "assistant", "content": "kept",
             "reasoning_content": "mine"},
        ]}
        cached, backfilled = proxy._reasoning_replay("conv-r", body)
        self.assertEqual((cached, backfilled), (1, 1))
        self.assertEqual(body["messages"][1]["reasoning_content"],
                         "the chain of thought")
        self.assertEqual(body["messages"][3]["reasoning_content"], " ")
        self.assertEqual(body["messages"][4]["reasoning_content"], "mine")

    def test_observer_stores_streamed_reasoning(self):
        tap = proxy._ResponseTap(True)
        events = [
            {"choices": [{"delta": {"reasoning_content": "step 1; "}}]},
            {"choices": [{"delta": {"content": "I will call "}}]},
            {"choices": [{"delta": {"tool_calls": [
                {"index": 0, "id": "call_z",
                 "function": {"name": "get_time", "arguments": "{}"}}]}}]},
            {"choices": [{"delta": {"content": "get_time."}}]},
        ]
        for event in events:
            tap.feed(b"data: " + json.dumps(event).encode() + b"\n\n")
        proxy._reasoning_observe("conv-s", {"upstream": "deepseek"}, tap)
        body = {"messages": [
            {"role": "assistant", "content": "I will call get_time.",
             "tool_calls": [{"id": "call_z", "function": {
                 "name": "get_time", "arguments": "{}"}}]}]}
        cached, backfilled = proxy._reasoning_replay("conv-s", body)
        self.assertEqual((cached, backfilled), (1, 0))
        self.assertEqual(body["messages"][0]["reasoning_content"], "step 1; ")

    def test_observer_ignores_other_upstreams(self):
        tap = proxy._ResponseTap(False)
        tap.feed(json.dumps({"choices": [
            {"message": {"role": "assistant", "content": "x",
                         "reasoning_content": "r"}}]}).encode())
        proxy._reasoning_observe("conv-o", {"upstream": "openrouter"}, tap)
        cached, _ = proxy._reasoning_replay("conv-o", {"messages": [
            {"role": "assistant", "content": "x"}]})
        self.assertEqual(cached, 0)


if __name__ == "__main__":
    unittest.main()
