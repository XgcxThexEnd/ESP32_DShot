import math
import sys
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from stress_test import (  # noqa: E402
    PercentageObservation,
    StressTester,
    _json_safe,
    main,
    parse_broker_uri,
    percentile,
    scenario_result_errors,
)


class FakeClient:
    def __init__(self) -> None:
        self.next_mid = 1
        self.subscriptions = []
        self.published = []
        self.reject_next = False
        self.auto_puback = True
        self.on_connect = None
        self.on_disconnect = None
        self.on_message = None
        self.on_subscribe = None
        self.on_publish = None

    def username_pw_set(self, username, password) -> None:
        self.credentials = (username, password)

    def subscribe(self, topic, qos=0):
        mid = self.next_mid
        self.next_mid += 1
        self.subscriptions.append((mid, topic, qos))
        return 0, mid

    def publish(self, topic, payload, qos=0, retain=False):
        mid = self.next_mid
        self.next_mid += 1
        self.published.append((topic, payload, qos, retain))
        if self.reject_next:
            self.reject_next = False
            return SimpleNamespace(rc=4, mid=mid)
        if self.auto_puback and self.on_publish is not None:
            self.on_publish(self, None, mid)
        return SimpleNamespace(rc=0, mid=mid)

    def connect(self, host, port, keepalive=30) -> None:
        self.connection = (host, port, keepalive)

    def disconnect(self) -> None:
        return None

    def loop_start(self) -> None:
        return None

    def loop_stop(self) -> None:
        return None


def make_tester(*, seed: int = 7, legacy: bool = False) -> tuple[StressTester, FakeClient]:
    client = FakeClient()
    tester = StressTester(
        broker_uri="mqtt://localhost:1883",
        username=None,
        password=None,
        topic_prefix="greenhouse_esp",
        node_id=None if legacy else "node-a",
        legacy_topics=legacy,
        seed=seed,
        fan_indices=[1],
        client_factory=lambda client_id: client,
    )
    return tester, client


def message(tester: StressTester, leaf: str, payload: bytes) -> SimpleNamespace:
    return SimpleNamespace(topic=tester.fan_topic(1, leaf), payload=payload)


class StressToolUnitTests(unittest.TestCase):
    def test_parse_broker_uri(self) -> None:
        self.assertEqual(parse_broker_uri("mqtt://broker"), ("broker", 1883, False))
        self.assertEqual(parse_broker_uri("mqtts://broker:9443"), ("broker", 9443, True))
        self.assertEqual(parse_broker_uri("[::1]:1884"), ("::1", 1884, False))
        for uri in (
            "", "http://broker", "mqtt://user:pass@broker",
            "mqtt://broker/path", "mqtt://broker:0",
        ):
            with self.subTest(uri=uri), self.assertRaises(ValueError):
                parse_broker_uri(uri)

    def test_node_scope_is_default_and_requires_node_id(self) -> None:
        tester, _ = make_tester()
        self.assertEqual(tester.topic_prefix, "greenhouse_esp/nodes/node-a")
        self.assertEqual(
            tester.fan_topic(1, "percentage/set"),
            "greenhouse_esp/nodes/node-a/fan1/percentage/set",
        )
        with self.assertRaisesRegex(ValueError, "node ID is required"):
            StressTester(
                broker_uri="mqtt://localhost",
                username=None,
                password=None,
                topic_prefix="greenhouse_esp",
                fan_indices=[1],
                client_factory=lambda client_id: FakeClient(),
            )

    def test_legacy_topics_are_explicit(self) -> None:
        tester, _ = make_tester(legacy=True)
        self.assertEqual(tester.topic_prefix, "greenhouse_esp")
        self.assertEqual(tester.fan_topic(1, "set"), "greenhouse_esp/fan1/set")
        result = tester._command_accounting(1, None, 3, timeout=0)
        self.assertFalse(result["accounting_supported"])
        self.assertIn("legacy", result["accounting_reason"])

    def test_topic_identifiers_are_restricted_like_firmware(self) -> None:
        for root, node in (("bad/root", "node"), ("root", "bad+node"), ("root", "nødé")):
            with self.subTest(root=root, node=node), self.assertRaises(ValueError):
                StressTester(
                    broker_uri="mqtt://localhost",
                    username=None,
                    password=None,
                    topic_prefix=root,
                    node_id=node,
                    fan_indices=[1],
                    client_factory=lambda client_id: FakeClient(),
                )

    def test_publish_rc_and_puback_are_tracked(self) -> None:
        tester, client = make_tester()
        self.assertTrue(tester.pub_pct(1, 37, qos=1))
        self.assertEqual(tester.publish_stats()["pubacked"], 1)
        client.reject_next = True
        self.assertFalse(tester.pub_set(1, True, qos=0))
        stats = tester.publish_stats()
        self.assertEqual(stats["attempted"], 2)
        self.assertEqual(stats["accepted"], 1)
        self.assertEqual(stats["rejected"], 1)
        self.assertEqual(stats["pending_pubacks"], 0)

    def test_missing_puback_is_reported(self) -> None:
        tester, client = make_tester()
        client.auto_puback = False
        before = tester.publish_stats()
        self.assertTrue(tester.pub_pct(1, 20, qos=1))
        outcome = tester._finish_publish_window(before, timeout=0)
        self.assertEqual(outcome["missing_pubacks"], 1)
        errors = scenario_result_errors(
            "latency",
            {
                "publish": outcome,
                "publish_failures": 0,
                "first_timeouts": 0,
                "settle_timeouts": 0,
                "first_p95": 0.1,
                "settle_p95": 0.2,
                "stopped": True,
            },
            [1],
        )
        self.assertTrue(any("PUBACK" in error for error in errors))

    def test_subscription_waits_for_all_node_topics(self) -> None:
        tester, client = make_tester()
        tester._on_connect(client, None, None, 0)
        topics = {topic for _mid, topic, _qos in client.subscriptions}
        self.assertIn("greenhouse_esp/nodes/node-a/fan1/accepted_command_count", topics)
        self.assertIn("greenhouse_esp/nodes/node-a/status", topics)
        self.assertEqual(len(topics), 5)
        self.assertFalse(tester._subscriptions_done.is_set())
        for mid, _topic, _qos in client.subscriptions:
            tester._on_subscribe(client, None, mid, [1])
        self.assertTrue(tester._subscriptions_done.is_set())

    def test_wait_percentage_requires_fresh_message(self) -> None:
        tester, _ = make_tester()
        msg = message(tester, "percentage", b"37")
        tester._on_message(None, None, msg)
        _, sequence, _ = tester.percentage_snapshot(1)
        self.assertIsNone(
            tester.wait_percentage(1, lambda value: value == 37, after_sequence=sequence, timeout=0)
        )
        timer = threading.Timer(0.01, tester._on_message, args=(None, None, msg))
        timer.start()
        try:
            observation = tester.wait_percentage(
                1, lambda value: value == 37, after_sequence=sequence, timeout=0.5
            )
        finally:
            timer.join()
        self.assertIsNotNone(observation)

    def test_accepted_counter_accounts_handled_and_lost(self) -> None:
        tester, _ = make_tester()
        tester._on_message(None, None, message(tester, "accepted_command_count", b"10"))
        before = tester.accepted_snapshot(1)
        tester._on_message(None, None, message(tester, "accepted_command_count", b"13"))
        handled = tester._command_accounting(1, before, 3, timeout=0)
        self.assertEqual(handled["handled_ratio"], 1.0)
        self.assertEqual(handled["lost_commands"], 0)

        tester._on_message(None, None, message(tester, "accepted_command_count", b"20"))
        before = tester.accepted_snapshot(1)
        tester._on_message(None, None, message(tester, "accepted_command_count", b"22"))
        lost = tester._command_accounting(1, before, 4, timeout=0)
        self.assertEqual(lost["lost_commands"], 2)
        self.assertEqual(lost["handled_ratio"], 0.5)

    def test_accounting_validator_never_uses_heartbeat_as_success(self) -> None:
        result = {
            "sent": 10,
            "client_rejected": 0,
            "fresh_observations": 999,
            "accounting_supported": False,
            "accounting_reason": "no retained counter",
            "stopped": True,
        }
        errors = scenario_result_errors("spam", result, [1])
        self.assertTrue(any("accounting unsupported" in error for error in errors))

    def test_random_workload_is_seeded(self) -> None:
        first, _ = make_tester(seed=1234)
        second, _ = make_tester(seed=1234)
        third, _ = make_tester(seed=1235)
        a = [first.rng.randint(0, 100) for _ in range(10)]
        self.assertEqual(a, [second.rng.randint(0, 100) for _ in range(10)])
        self.assertNotEqual(a, [third.rng.randint(0, 100) for _ in range(10)])

    def test_percentile_and_json_are_strict(self) -> None:
        self.assertTrue(math.isinf(percentile([], 95)))
        self.assertEqual(percentile([0.1, 0.2, 0.3], 50), 0.2)
        self.assertEqual(_json_safe({"timeout": math.inf, 1: [math.nan]}), {"timeout": None, "1": [None]})

    def test_invalid_scenario_keeps_strict_payload_cases(self) -> None:
        tester, client = make_tester()
        tester.command_percentage = lambda idx, pct, timeout=8.0: PercentageObservation(  # type: ignore[method-assign]
            pct, 1, 1.0
        )
        # Alternate changed-state waits (None) and liveness waits (baseline).
        calls = 0
        def wait(*args, **kwargs):
            nonlocal calls
            del args, kwargs
            calls += 1
            return None if calls % 2 else PercentageObservation(37, calls, float(calls))
        tester.wait_percentage = wait  # type: ignore[method-assign]
        result = tester.scenario_invalid(1, observation_window=0, confirmation_timeout=0)
        payloads = {payload for _topic, payload, _qos, _retain in client.published}
        self.assertTrue({" 50", "50 ", "+1"}.issubset(payloads))
        self.assertEqual(result["failures"], [])

    def test_main_rejects_empty_scenarios_before_connect(self) -> None:
        argv = [
            "stress_test.py", "--broker", "mqtt://localhost", "--node-id", "node-a",
            "--scenarios", "",
        ]
        with patch.object(sys, "argv", argv), self.assertRaises(SystemExit) as raised:
            main()
        self.assertEqual(raised.exception.code, 2)

    def test_direct_argument_validation(self) -> None:
        tester, _ = make_tester()
        with self.assertRaises(ValueError):
            tester.scenario_spam(1, rate_hz=0, duration_s=1)
        with self.assertRaises(ValueError):
            tester.scenario_multi_mix(rate_hz=1, duration_s=0)
        with self.assertRaises(ValueError):
            tester.pub_pct(2, 10)


if __name__ == "__main__":
    unittest.main()
