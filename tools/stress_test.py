#!/usr/bin/env python3
"""MQTT stress tester for the greenhouse DShot controller.

This commands real actuators. Use only on a mechanically safe rig. Node-scoped
topics (``<root>/nodes/<node-id>``) are the default. ``--legacy-topics`` is an
explicit migration mode; legacy firmware cannot provide loss accounting.
"""

from __future__ import annotations

import argparse
import json
import logging
import math
import os
import random
import secrets
import sys
import threading
import time
from collections import defaultdict, deque
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Deque, Dict, List, Optional, Set, Tuple
from urllib.parse import urlsplit

try:
    import paho.mqtt.client as mqtt  # type: ignore
except ImportError:  # Keep offline unit tests importable without Paho.
    mqtt = None  # type: ignore[assignment]


UINT32_MODULUS = 1 << 32


def _env_flag(name: str, default: bool = False) -> bool:
    value = os.getenv(name)
    return default if value is None else value.strip().lower() in {"1", "true", "yes", "on"}


def _topic_level_valid(value: str) -> bool:
    return bool(value) and all(ch.isascii() and (ch.isalnum() or ch in "_-") for ch in value)


def parse_broker_uri(uri: str) -> Tuple[str, int, bool]:
    """Return ``(host, port, secure)`` for an MQTT broker address."""
    value = uri.strip()
    if not value:
        raise ValueError("broker URI must not be empty")
    if "://" not in value:
        value = f"mqtt://{value}"
    parsed = urlsplit(value)
    if parsed.scheme not in {"mqtt", "mqtts"}:
        raise ValueError(f"unsupported broker scheme: {parsed.scheme!r}")
    if parsed.username is not None or parsed.password is not None:
        raise ValueError("put MQTT credentials in --username/--password, not the broker URI")
    if parsed.path not in {"", "/"} or parsed.query or parsed.fragment:
        raise ValueError("broker URI must not contain a path, query, or fragment")
    if parsed.hostname is None:
        raise ValueError("broker URI is missing a host")
    try:
        port = parsed.port
    except ValueError as exc:
        raise ValueError("broker URI contains an invalid port") from exc
    if port is None:
        port = 8883 if parsed.scheme == "mqtts" else 1883
    if not 1 <= port <= 65535:
        raise ValueError("broker port must be from 1 through 65535")
    return parsed.hostname, port, parsed.scheme == "mqtts"


@dataclass
class FanState:
    percentage: Optional[int] = None
    state: Optional[str] = None
    rmt_refresh_count: Optional[int] = None
    accepted_command_count: Optional[int] = None
    percentage_seq: int = 0
    state_seq: int = 0
    rmt_refresh_seq: int = 0
    accepted_command_seq: int = 0
    percentage_update_ts: float = 0.0
    state_update_ts: float = 0.0
    rmt_refresh_update_ts: float = 0.0
    accepted_command_update_ts: float = 0.0


@dataclass(frozen=True)
class PercentageObservation:
    value: int
    sequence: int
    timestamp: float


@dataclass(frozen=True)
class CounterObservation:
    value: int
    sequence: int
    timestamp: float


class StressTester:
    def __init__(
        self,
        *,
        broker_uri: str,
        username: Optional[str],
        password: Optional[str],
        topic_prefix: str,
        fan_indices: List[int],
        node_id: Optional[str] = None,
        legacy_topics: bool = False,
        seed: int = 0,
        keepalive: int = 30,
        client_factory: Optional[Callable[[str], object]] = None,
    ) -> None:
        if not fan_indices:
            raise ValueError("at least one fan index is required")
        if len(set(fan_indices)) != len(fan_indices):
            raise ValueError("fan indices must be unique")
        if any(idx < 1 for idx in fan_indices):
            raise ValueError("fan indices must be at least 1")
        root_topic = topic_prefix.strip("/")
        if not _topic_level_valid(root_topic):
            raise ValueError("root topic must be one ASCII level using letters, digits, '_' or '-'")
        if node_id is not None and not _topic_level_valid(node_id):
            raise ValueError("node ID must use only ASCII letters, digits, '_' or '-'")
        if not legacy_topics and not node_id:
            raise ValueError("node ID is required unless --legacy-topics is used")

        self.root_topic = root_topic
        self.node_id = node_id
        self.legacy_topics = legacy_topics
        self.topic_prefix = root_topic if legacy_topics else f"{root_topic}/nodes/{node_id}"
        self.fan_indices = list(fan_indices)
        self.seed = int(seed)
        self.rng = random.Random(self.seed)
        self.host, self.port, self.secure = parse_broker_uri(broker_uri)
        self.keepalive = keepalive

        client_id = f"stress_{secrets.token_hex(4)}"
        if client_factory is not None:
            self.client = client_factory(client_id)
        else:
            if mqtt is None:
                raise RuntimeError(
                    "Missing paho-mqtt; install with: python -m pip install -r tools/requirements.txt"
                )
            self.client = mqtt.Client(client_id=client_id)
        if username:
            self.client.username_pw_set(username, password)
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message
        self.client.on_subscribe = self._on_subscribe
        self.client.on_publish = self._on_publish

        self._conn_event = threading.Event()
        self._connected = threading.Event()
        self._subscriptions_done = threading.Event()
        self._connect_error: Optional[str] = None
        self._subscription_error: Optional[str] = None
        self._pending_subscriptions: Set[int] = set()
        self._early_subacks: Set[int] = set()
        self._subscribing = False
        self._subscription_lock = threading.RLock()

        self._publish_lock = threading.RLock()
        self._publish_cond = threading.Condition(self._publish_lock)
        self._pending_pubacks: Set[int] = set()
        self._early_pubacks: Set[int] = set()
        self._qos0_callbacks: Set[int] = set()
        self._publish_attempted = 0
        self._publish_accepted = 0
        self._publish_rejected = 0
        self._qos1_accepted = 0
        self._qos1_pubacked = 0

        self.states: Dict[int, FanState] = defaultdict(FanState)
        self._cond = threading.Condition()
        self._msg_queue: Deque[Tuple[str, str]] = deque(maxlen=1000)

    def _require_fan(self, idx: int) -> None:
        if idx not in self.fan_indices:
            raise ValueError(f"fan{idx} is not in the configured fan set")

    def fan_topic(self, idx: int, leaf: str) -> str:
        self._require_fan(idx)
        return f"{self.topic_prefix}/fan{idx}/{leaf}"

    # MQTT callbacks
    def _on_connect(self, client, userdata, flags, rc, *args):  # type: ignore[no-untyped-def]
        del client, userdata, flags, args
        self._connect_error = None
        if int(rc) != 0:
            self._connect_error = f"MQTT connection rejected with rc={rc}"
            self._conn_event.set()
            return
        self._connected.set()
        self._subscriptions_done.clear()
        self._subscription_error = None
        topics: List[str] = []
        for idx in self.fan_indices:
            topics.extend([
                self.fan_topic(idx, "state"),
                self.fan_topic(idx, "percentage"),
                self.fan_topic(idx, "rmt_refresh_count"),
            ])
            if not self.legacy_topics:
                topics.append(self.fan_topic(idx, "accepted_command_count"))
        topics.append(
            f"{self.topic_prefix}/status" if not self.legacy_topics
            else f"{self.topic_prefix}/status/+"
        )
        with self._subscription_lock:
            self._pending_subscriptions.clear()
            self._early_subacks.clear()
            self._subscribing = True
            for topic in topics:
                result, mid = self.client.subscribe(topic, qos=1)
                if int(result) != 0:
                    self._subscription_error = f"subscribe failed for {topic!r}, rc={result}"
                elif mid in self._early_subacks:
                    self._early_subacks.remove(mid)
                else:
                    self._pending_subscriptions.add(mid)
            self._subscribing = False
            if not self._pending_subscriptions:
                self._subscriptions_done.set()
        self._conn_event.set()

    def _on_disconnect(self, client, userdata, rc, *args):  # type: ignore[no-untyped-def]
        del client, userdata, rc, args
        self._connected.clear()

    def _on_subscribe(self, client, userdata, mid, granted_qos, *args):  # type: ignore[no-untyped-def]
        del client, userdata, args
        if any(int(qos) == 128 for qos in granted_qos):
            self._subscription_error = f"broker rejected subscription mid={mid}"
        with self._subscription_lock:
            if mid in self._pending_subscriptions:
                self._pending_subscriptions.remove(mid)
            elif self._subscribing:
                self._early_subacks.add(mid)
            if not self._subscribing and not self._pending_subscriptions:
                self._subscriptions_done.set()

    def _on_publish(self, client, userdata, mid, *args):  # type: ignore[no-untyped-def]
        del client, userdata, args
        with self._publish_cond:
            if mid in self._pending_pubacks:
                self._pending_pubacks.remove(mid)
                self._qos1_pubacked += 1
            elif mid in self._qos0_callbacks:
                self._qos0_callbacks.remove(mid)
            else:
                self._early_pubacks.add(mid)
            self._publish_cond.notify_all()

    def _on_message(self, client, userdata, msg):  # type: ignore[no-untyped-def]
        del client, userdata
        topic = str(msg.topic)
        try:
            payload = msg.payload.decode("utf-8", errors="strict")
        except UnicodeDecodeError:
            logging.warning("Non-UTF-8 MQTT payload on %s", topic)
            return
        timestamp = time.monotonic()
        self._msg_queue.append((topic, payload))
        for idx in self.fan_indices:
            if topic == self.fan_topic(idx, "percentage"):
                try:
                    value = int(payload)
                except ValueError:
                    logging.warning("Invalid percentage observation on %s: %r", topic, payload)
                    return
                with self._cond:
                    state = self.states[idx]
                    state.percentage = value
                    state.percentage_seq += 1
                    state.percentage_update_ts = timestamp
                    self._cond.notify_all()
                return
            if topic == self.fan_topic(idx, "state"):
                with self._cond:
                    state = self.states[idx]
                    state.state = payload
                    state.state_seq += 1
                    state.state_update_ts = timestamp
                    self._cond.notify_all()
                return
            if topic == self.fan_topic(idx, "rmt_refresh_count"):
                self._record_counter(idx, "rmt", payload, timestamp, topic)
                return
            if not self.legacy_topics and topic == self.fan_topic(idx, "accepted_command_count"):
                self._record_counter(idx, "accepted", payload, timestamp, topic)
                return

    def _record_counter(self, idx: int, kind: str, payload: str, timestamp: float, topic: str) -> None:
        try:
            value = int(payload)
            if not 0 <= value < UINT32_MODULUS:
                raise ValueError
        except ValueError:
            logging.warning("Invalid %s counter observation on %s: %r", kind, topic, payload)
            return
        with self._cond:
            state = self.states[idx]
            if kind == "rmt":
                state.rmt_refresh_count = value
                state.rmt_refresh_seq += 1
                state.rmt_refresh_update_ts = timestamp
            else:
                state.accepted_command_count = value
                state.accepted_command_seq += 1
                state.accepted_command_update_ts = timestamp
            self._cond.notify_all()

    # Connection and publish helpers
    def connect(self) -> None:
        if self.secure:
            self.client.tls_set()
        self._conn_event.clear()
        self.client.connect(self.host, self.port, keepalive=self.keepalive)
        self.client.loop_start()
        try:
            if not self._conn_event.wait(10):
                raise TimeoutError("timeout waiting for MQTT CONNACK")
            if self._connect_error:
                raise ConnectionError(self._connect_error)
            if not self._subscriptions_done.wait(5):
                raise TimeoutError("timeout waiting for MQTT SUBACK messages")
            if self._subscription_error:
                raise ConnectionError(self._subscription_error)
        except Exception:
            try:
                self.client.disconnect()
            finally:
                self.client.loop_stop()
            raise

    def disconnect(self, *, stop_fans: bool = True, stop_timeout: float = 8.0) -> Dict[int, bool]:
        stopped: Dict[int, bool] = {}
        try:
            if stop_fans:
                stopped = self.stop_all(timeout=stop_timeout)
            if not self.wait_for_pubacks(5.0):
                logging.error("Timed out waiting for %d MQTT PUBACK(s)", len(self._pending_pubacks))
        finally:
            try:
                self.client.disconnect()
            finally:
                self.client.loop_stop()
                self._connected.clear()
        return stopped

    @staticmethod
    def _message_info(result: object) -> Tuple[int, Optional[int]]:
        rc = getattr(result, "rc", None)
        mid = getattr(result, "mid", None)
        if rc is None and isinstance(result, tuple) and result:
            rc = result[0]
            mid = result[1] if len(result) > 1 else mid
        return int(rc) if rc is not None else -1, int(mid) if mid is not None else None

    def _publish(self, topic: str, payload: str, *, qos: int, retain: bool = False) -> bool:
        with self._publish_cond:
            self._publish_attempted += 1
            try:
                result = self.client.publish(topic, payload, qos=qos, retain=retain)
            except Exception:
                self._publish_rejected += 1
                logging.exception("MQTT publish raised for %s", topic)
                return False
            rc, mid = self._message_info(result)
            if rc != 0 or (qos > 0 and mid is None):
                self._publish_rejected += 1
                logging.error("MQTT publish rejected for %s (rc=%d, mid=%r)", topic, rc, mid)
                return False
            self._publish_accepted += 1
            if qos > 0:
                assert mid is not None
                self._qos1_accepted += 1
                if mid in self._early_pubacks:
                    self._early_pubacks.remove(mid)
                    self._qos1_pubacked += 1
                else:
                    self._pending_pubacks.add(mid)
            elif mid is not None:
                if mid in self._early_pubacks:
                    self._early_pubacks.remove(mid)
                else:
                    self._qos0_callbacks.add(mid)
            return True

    def pub_pct(self, idx: int, pct: int, qos: int = 0) -> bool:
        return self._publish(self.fan_topic(idx, "percentage/set"), str(pct), qos=qos)

    def pub_set(self, idx: int, on: bool, qos: int = 0) -> bool:
        return self._publish(self.fan_topic(idx, "set"), "ON" if on else "OFF", qos=qos)

    def _pub_raw(self, topic: str, payload: str, qos: int = 1) -> bool:
        return self._publish(topic, payload, qos=qos)

    def wait_for_pubacks(self, timeout: float) -> bool:
        deadline = time.monotonic() + max(0.0, timeout)
        with self._publish_cond:
            while self._pending_pubacks:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return False
                self._publish_cond.wait(min(0.2, remaining))
            return True

    def publish_stats(self) -> Dict[str, int]:
        with self._publish_lock:
            return {
                "attempted": self._publish_attempted,
                "accepted": self._publish_accepted,
                "rejected": self._publish_rejected,
                "qos1_accepted": self._qos1_accepted,
                "pubacked": self._qos1_pubacked,
                "pending_pubacks": len(self._pending_pubacks),
            }

    def _finish_publish_window(self, before: Dict[str, int], timeout: float = 5.0) -> Dict[str, int]:
        self.wait_for_pubacks(timeout)
        after = self.publish_stats()
        result = {key: after[key] - before[key] for key in before if key != "pending_pubacks"}
        result["missing_pubacks"] = max(0, result["qos1_accepted"] - result["pubacked"])
        return result

    # Observation and accounting helpers
    def percentage_snapshot(self, idx: int) -> Tuple[Optional[int], int, float]:
        self._require_fan(idx)
        with self._cond:
            state = self.states[idx]
            return state.percentage, state.percentage_seq, state.percentage_update_ts

    def rmt_snapshot(self, idx: int) -> Tuple[Optional[int], int, float]:
        self._require_fan(idx)
        with self._cond:
            state = self.states[idx]
            return state.rmt_refresh_count, state.rmt_refresh_seq, state.rmt_refresh_update_ts

    def accepted_snapshot(self, idx: int, timeout: float = 0.0) -> Optional[CounterObservation]:
        self._require_fan(idx)
        deadline = time.monotonic() + max(0.0, timeout)
        with self._cond:
            while self.states[idx].accepted_command_count is None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
                self._cond.wait(min(0.2, remaining))
            state = self.states[idx]
            assert state.accepted_command_count is not None
            return CounterObservation(
                state.accepted_command_count,
                state.accepted_command_seq,
                state.accepted_command_update_ts,
            )

    def wait_percentage(
        self,
        idx: int,
        predicate: Callable[[int], bool],
        *,
        after_sequence: int,
        not_before: float = 0.0,
        timeout: float,
    ) -> Optional[PercentageObservation]:
        self._require_fan(idx)
        deadline = time.monotonic() + max(0.0, timeout)
        with self._cond:
            while True:
                state = self.states[idx]
                if (state.percentage is not None and state.percentage_seq > after_sequence
                        and state.percentage_update_ts >= not_before and predicate(state.percentage)):
                    return PercentageObservation(
                        state.percentage, state.percentage_seq, state.percentage_update_ts
                    )
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
                self._cond.wait(min(0.2, remaining))

    def _wait_rmt_round(
        self, after_sequences: Dict[int, int], timeout: float
    ) -> Dict[int, CounterObservation]:
        deadline = time.monotonic() + max(0.0, timeout)
        with self._cond:
            while True:
                observations: Dict[int, CounterObservation] = {}
                for idx in self.fan_indices:
                    state = self.states[idx]
                    if state.rmt_refresh_count is not None and state.rmt_refresh_seq > after_sequences[idx]:
                        observations[idx] = CounterObservation(
                            state.rmt_refresh_count, state.rmt_refresh_seq, state.rmt_refresh_update_ts
                        )
                if len(observations) == len(self.fan_indices):
                    return observations
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return observations
                self._cond.wait(min(0.2, remaining))

    @staticmethod
    def _counter_delta(before: int, after: int) -> Tuple[int, bool]:
        if after >= before:
            return after - before, False
        if before >= 0xFFFF0000 and after <= 0x0000FFFF:
            return (after - before) % UINT32_MODULUS, False
        return 0, True

    def _command_accounting(
        self,
        idx: int,
        before: Optional[CounterObservation],
        accepted_commands: int,
        timeout: float = 10.0,
    ) -> Dict[str, object]:
        base: Dict[str, object] = {
            "accepted_commands": accepted_commands,
            "handled_commands": 0,
            "lost_commands": accepted_commands,
            "unexpected_commands": 0,
            "handled_ratio": 0.0 if accepted_commands else 1.0,
            "counter_reset": False,
        }
        if self.legacy_topics:
            return {
                **base,
                "accounting_supported": False,
                "accounting_reason": "legacy topics do not expose accepted_command_count",
            }
        if before is None:
            return {
                **base,
                "accounting_supported": False,
                "accounting_reason": f"no retained {self.fan_topic(idx, 'accepted_command_count')} baseline",
            }
        deadline = time.monotonic() + max(0.0, timeout)
        after = before
        handled = 0
        reset = False
        with self._cond:
            while True:
                state = self.states[idx]
                if state.accepted_command_count is not None and state.accepted_command_seq > before.sequence:
                    after = CounterObservation(
                        state.accepted_command_count,
                        state.accepted_command_seq,
                        state.accepted_command_update_ts,
                    )
                    handled, reset = self._counter_delta(before.value, after.value)
                    if reset or handled >= accepted_commands:
                        break
                if accepted_commands == 0:
                    break
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                self._cond.wait(min(0.2, remaining))
        return {
            **base,
            "accounting_supported": True,
            "accounting_reason": None,
            "counter_before": before.value,
            "counter_after": after.value,
            "handled_commands": handled,
            "lost_commands": max(0, accepted_commands - handled),
            "unexpected_commands": max(0, handled - accepted_commands),
            "handled_ratio": min(handled, accepted_commands) / accepted_commands if accepted_commands else 1.0,
            "counter_reset": reset,
        }

    # Safe command helpers
    def command_percentage(self, idx: int, pct: int, timeout: float = 8.0) -> Optional[PercentageObservation]:
        _, sequence, _ = self.percentage_snapshot(idx)
        command_time = time.monotonic()
        if not self.pub_pct(idx, pct, qos=1):
            return None
        return self.wait_percentage(
            idx, lambda value: value == pct,
            after_sequence=sequence, not_before=command_time, timeout=timeout,
        )

    def stop_all(self, timeout: float = 8.0) -> Dict[int, bool]:
        if not self._connected.is_set():
            return {idx: False for idx in self.fan_indices}
        self._pub_raw(f"{self.topic_prefix}/schedule/override", "1", qos=1)
        sequences = {idx: self.percentage_snapshot(idx)[1] for idx in self.fan_indices}
        command_times: Dict[int, float] = {}
        published: Dict[int, bool] = {}
        for idx in self.fan_indices:
            command_times[idx] = time.monotonic()
            published[idx] = self.pub_pct(idx, 0, qos=1)
        deadline = time.monotonic() + timeout
        stopped: Dict[int, bool] = {}
        for idx in self.fan_indices:
            observation = self.wait_percentage(
                idx, lambda value: value == 0,
                after_sequence=sequences[idx], not_before=command_times[idx],
                timeout=max(0.0, deadline - time.monotonic()),
            ) if published[idx] else None
            stopped[idx] = observation is not None
        return stopped

    # Scenarios
    def scenario_latency(self, idx: int, iterations: int = 10) -> Dict[str, object]:
        self._require_fan(idx)
        if iterations < 1:
            raise ValueError("iterations must be at least 1")
        publish_before = self.publish_stats()
        first_latencies: List[float] = []
        settle_latencies: List[float] = []
        targets: List[int] = []
        first_timeouts = settle_timeouts = publish_failures = 0
        try:
            for _ in range(iterations):
                target = self.rng.randint(0, 100)
                targets.append(target)
                _, sequence, _ = self.percentage_snapshot(idx)
                started = time.monotonic()
                if not self.pub_pct(idx, target, qos=1):
                    publish_failures += 1
                    first_timeouts += 1
                    settle_timeouts += 1
                    first_latencies.append(math.inf)
                    settle_latencies.append(math.inf)
                    continue
                first = self.wait_percentage(
                    idx, lambda value: 0 <= value <= 100,
                    after_sequence=sequence, not_before=started, timeout=3.0,
                )
                if first is None:
                    first_timeouts += 1
                    first_latencies.append(math.inf)
                else:
                    first_latencies.append(max(0.0, first.timestamp - started))
                settled = self.wait_percentage(
                    idx, lambda value, target=target: value == target,
                    after_sequence=sequence, not_before=started,
                    timeout=max(0.0, 8.0 - (time.monotonic() - started)),
                )
                if settled is None:
                    settle_timeouts += 1
                    settle_latencies.append(math.inf)
                else:
                    settle_latencies.append(max(0.0, settled.timestamp - started))
        finally:
            stopped = self.command_percentage(idx, 0) is not None
        return {
            "iterations": iterations,
            "targets": targets,
            "first_p50": percentile(first_latencies, 50),
            "first_p95": percentile(first_latencies, 95),
            "settle_p50": percentile(settle_latencies, 50),
            "settle_p95": percentile(settle_latencies, 95),
            "first_timeouts": first_timeouts,
            "settle_timeouts": settle_timeouts,
            "publish_failures": publish_failures,
            "stopped": stopped,
            "publish": self._finish_publish_window(publish_before),
        }

    def scenario_toggle_storm(self, idx: int, cycles: int = 50, interval: float = 0.05) -> Dict[str, object]:
        self._require_fan(idx)
        if cycles < 0 or interval < 0:
            raise ValueError("cycles and interval must not be negative")
        publish_before = self.publish_stats()
        before_sequence: Optional[int] = None
        accounting_before: Optional[CounterObservation] = None
        on_observation: Optional[PercentageObservation] = None
        off_observation: Optional[PercentageObservation] = None
        flips = accepted = rejected = 0
        try:
            _, sequence, _ = self.percentage_snapshot(idx)
            command_time = time.monotonic()
            if self.pub_set(idx, True, qos=1):
                on_observation = self.wait_percentage(
                    idx, lambda value: value > 0,
                    after_sequence=sequence, not_before=command_time, timeout=8.0,
                )
            _, sequence, _ = self.percentage_snapshot(idx)
            command_time = time.monotonic()
            if self.pub_set(idx, False, qos=1):
                off_observation = self.wait_percentage(
                    idx, lambda value: value == 0,
                    after_sequence=sequence, not_before=command_time, timeout=8.0,
                )
            _, before_sequence, _ = self.percentage_snapshot(idx)
            accounting_before = self.accepted_snapshot(idx, timeout=2.0)
            if on_observation is not None and off_observation is not None:
                for _ in range(cycles):
                    for value in (True, False):
                        flips += 1
                        if self.pub_set(idx, value, qos=1):
                            accepted += 1
                        else:
                            rejected += 1
                    if interval:
                        time.sleep(interval)
        finally:
            _, after_sequence, _ = self.percentage_snapshot(idx)
            fresh = max(0, after_sequence - before_sequence) if before_sequence is not None else 0
            accounting = self._command_accounting(idx, accounting_before, accepted)
            stopped = self.command_percentage(idx, 0) is not None
        return {
            "flips": flips,
            "client_accepted": accepted,
            "client_rejected": rejected,
            "fresh_observations": fresh,
            "representative_on": on_observation is not None,
            "representative_off": off_observation is not None,
            **accounting,
            "stopped": stopped,
            "publish": self._finish_publish_window(publish_before),
        }

    def scenario_spam(self, idx: int, rate_hz: int = 50, duration_s: int = 10) -> Dict[str, object]:
        self._require_fan(idx)
        if rate_hz < 1 or duration_s < 1:
            raise ValueError("rate_hz and duration_s must be at least 1")
        publish_before = self.publish_stats()
        _, before_sequence, _ = self.percentage_snapshot(idx)
        accounting_before = self.accepted_snapshot(idx, timeout=2.0)
        attempted = accepted = rejected = 0
        period = 1.0 / rate_hz
        end = time.monotonic() + duration_s
        next_publish = time.monotonic()
        try:
            while time.monotonic() < end:
                attempted += 1
                if self.pub_pct(idx, self.rng.randint(0, 100), qos=1):
                    accepted += 1
                else:
                    rejected += 1
                next_publish += period
                time.sleep(max(0.0, next_publish - time.monotonic()))
        finally:
            _, after_sequence, _ = self.percentage_snapshot(idx)
            fresh = max(0, after_sequence - before_sequence)
            accounting = self._command_accounting(idx, accounting_before, accepted)
            stopped = self.command_percentage(idx, 0) is not None
        return {
            "sent": attempted,
            "client_accepted": accepted,
            "client_rejected": rejected,
            "fresh_observations": fresh,
            **accounting,
            "stopped": stopped,
            "publish": self._finish_publish_window(publish_before),
        }

    def scenario_multi_mix(self, rate_hz: int = 20, duration_s: int = 15) -> Dict[str, object]:
        if rate_hz < 1 or duration_s < 1:
            raise ValueError("rate_hz and duration_s must be at least 1")
        publish_before = self.publish_stats()
        before_sequences = {idx: self.percentage_snapshot(idx)[1] for idx in self.fan_indices}
        counter_before = {idx: self.accepted_snapshot(idx, timeout=2.0) for idx in self.fan_indices}
        attempted = 0
        accepted = {idx: 0 for idx in self.fan_indices}
        rejected = {idx: 0 for idx in self.fan_indices}
        period = 1.0 / rate_hz
        end = time.monotonic() + duration_s
        next_publish = time.monotonic()
        try:
            while time.monotonic() < end:
                for idx in self.fan_indices:
                    attempted += 1
                    if self.rng.random() < 0.3:
                        ok = self.pub_set(idx, bool(self.rng.getrandbits(1)), qos=1)
                    else:
                        ok = self.pub_pct(idx, self.rng.randint(0, 100), qos=1)
                    if ok:
                        accepted[idx] += 1
                    else:
                        rejected[idx] += 1
                next_publish += period
                time.sleep(max(0.0, next_publish - time.monotonic()))
        finally:
            fresh = {
                idx: max(0, self.percentage_snapshot(idx)[1] - before_sequences[idx])
                for idx in self.fan_indices
            }
            accounting = {
                idx: self._command_accounting(idx, counter_before[idx], accepted[idx])
                for idx in self.fan_indices
            }
            stopped = self.stop_all()
        return {
            "sent": attempted,
            "client_accepted": accepted,
            "client_rejected": rejected,
            "fresh_observations": fresh,
            "fan_accounting": accounting,
            "stopped": stopped,
            "publish": self._finish_publish_window(publish_before),
        }

    def scenario_invalid(
        self,
        idx: int,
        *,
        baseline: int = 37,
        observation_window: float = 0.75,
        confirmation_timeout: float = 4.0,
    ) -> Dict[str, object]:
        self._require_fan(idx)
        publish_before = self.publish_stats()
        invalid_commands = [
            (self.fan_topic(idx, "percentage/set"), payload)
            for payload in [
                "banana", "-1", "101", "", "NaN", " ", "255", "-9999",
                "10.5", "50%", " 50", "50 ", "+1",
            ]
        ]
        invalid_commands.extend(
            (self.fan_topic(idx, "set"), payload)
            for payload in ["banana", "1", "", " ", "ONCE", "OFFLINE"]
        )
        failures: List[str] = []
        confirmations = sent = client_rejected = 0
        initial = self.command_percentage(idx, baseline, timeout=confirmation_timeout)
        if initial is None:
            stopped = self.command_percentage(idx, 0) is not None
            return {
                "sent": 0,
                "client_rejected": 0,
                "confirmed": 0,
                "failures": ["could not establish a fresh baseline observation"],
                "stopped": stopped,
                "publish": self._finish_publish_window(publish_before),
            }
        try:
            for topic, payload in invalid_commands:
                _, sequence, _ = self.percentage_snapshot(idx)
                command_time = time.monotonic()
                if self._pub_raw(topic, payload, qos=1):
                    sent += 1
                else:
                    client_rejected += 1
                    failures.append(f"client rejected publish to {topic} with payload {payload!r}")
                    continue
                changed = self.wait_percentage(
                    idx, lambda value, baseline=baseline: value != baseline,
                    after_sequence=sequence, not_before=command_time, timeout=observation_window,
                )
                if changed is not None:
                    failures.append(f"{topic} payload {payload!r} changed percentage to {changed.value}")
                _, sequence, _ = self.percentage_snapshot(idx)
                command_time = time.monotonic()
                if not self.pub_pct(idx, baseline, qos=1):
                    failures.append(f"client rejected baseline confirmation after {payload!r}")
                    continue
                confirmation = self.wait_percentage(
                    idx, lambda value, baseline=baseline: value == baseline,
                    after_sequence=sequence, not_before=command_time, timeout=confirmation_timeout,
                )
                if confirmation is None:
                    failures.append(f"no fresh baseline confirmation after {topic} payload {payload!r}")
                else:
                    confirmations += 1
        finally:
            stopped = self.command_percentage(idx, 0) is not None
        return {
            "sent": sent,
            "client_rejected": client_rejected,
            "confirmed": confirmations,
            "failures": failures,
            "stopped": stopped,
            "publish": self._finish_publish_window(publish_before),
        }

    def scenario_rmt_check(self, sample_timeout: float = 7.0) -> Dict[str, object]:
        start_sequences = {idx: self.rmt_snapshot(idx)[1] for idx in self.fan_indices}
        before = self._wait_rmt_round(start_sequences, timeout=sample_timeout)
        missing = [idx for idx in self.fan_indices if idx not in before]
        if missing:
            return {
                "missing": missing,
                "counter_resets": [],
                **{f"fan{idx}": None for idx in self.fan_indices},
            }
        after = self._wait_rmt_round(
            {idx: before[idx].sequence for idx in self.fan_indices}, timeout=sample_timeout
        )
        missing = [idx for idx in self.fan_indices if idx not in after]
        resets = [idx for idx in self.fan_indices if idx in after and after[idx].value < before[idx].value]
        result: Dict[str, object] = {"missing": missing, "counter_resets": resets}
        for idx in self.fan_indices:
            result[f"fan{idx}"] = (
                after[idx].value - before[idx].value if idx in after and idx not in resets else None
            )
        return result


def percentile(values: List[float], p: float) -> float:
    if not values:
        return math.inf
    values = sorted(values)
    index = max(0, min(len(values) - 1, int(round((p / 100.0) * (len(values) - 1)))))
    return values[index]


def _publish_result_errors(result: Dict[str, object]) -> List[str]:
    publish = result.get("publish")
    if publish is None:
        return []
    if not isinstance(publish, dict):
        return ["publish outcome is malformed"]
    errors: List[str] = []
    if publish.get("rejected") != 0:
        errors.append(f"MQTT client rejected {publish.get('rejected')} publish(es)")
    if publish.get("missing_pubacks") != 0:
        errors.append(f"{publish.get('missing_pubacks')} QoS 1 publish(es) lacked PUBACK")
    return errors


def _accounting_result_errors(result: Dict[str, object], minimum_handled_ratio: float) -> List[str]:
    if result.get("accounting_supported") is not True:
        return [f"command accounting unsupported: {result.get('accounting_reason', 'unknown reason')}"]
    errors: List[str] = []
    if result.get("counter_reset") is True:
        errors.append("accepted-command counter reset during the scenario")
    accepted = result.get("accepted_commands")
    if not isinstance(accepted, int) or accepted <= 0:
        errors.append("no commands were accepted by the MQTT client")
    ratio = result.get("handled_ratio")
    if not isinstance(ratio, (int, float)) or ratio < minimum_handled_ratio:
        errors.append(f"handled ratio {ratio!r} is below required {minimum_handled_ratio:.3f}")
    if result.get("unexpected_commands") not in {0, None}:
        errors.append(f"counter includes {result.get('unexpected_commands')} unexpected command(s)")
    return errors


def scenario_result_errors(
    scenario: str,
    result: Dict[str, object],
    fan_indices: List[int],
    *,
    latency_first_p95: float = 1.5,
    latency_settle_p95: float = 6.0,
    rmt_refresh_max: int = 5,
    minimum_handled_ratio: float = 1.0,
) -> List[str]:
    """Return validation failures for one scenario result."""
    errors = _publish_result_errors(result)
    if scenario == "latency":
        if result.get("publish_failures") != 0:
            errors.append("one or more latency commands were rejected by the client")
        if result.get("first_timeouts") != 0:
            errors.append("one or more first observations timed out")
        if result.get("settle_timeouts") != 0:
            errors.append("one or more target observations timed out")
        if not isinstance(result.get("first_p95"), (int, float)) or result["first_p95"] > latency_first_p95:
            errors.append(f"first_p95 exceeds {latency_first_p95}s")
        if not isinstance(result.get("settle_p95"), (int, float)) or result["settle_p95"] > latency_settle_p95:
            errors.append(f"settle_p95 exceeds {latency_settle_p95}s")
        if result.get("stopped") is not True:
            errors.append("final OFF was not confirmed")
    elif scenario in {"toggle", "spam"}:
        count_key = "flips" if scenario == "toggle" else "sent"
        if not isinstance(result.get(count_key), int) or result[count_key] <= 0:
            errors.append(f"{count_key} must be positive")
        if result.get("client_rejected") != 0:
            errors.append(f"MQTT client rejected {result.get('client_rejected')} flood command(s)")
        errors.extend(_accounting_result_errors(result, minimum_handled_ratio))
        if scenario == "toggle" and result.get("representative_on") is not True:
            errors.append("representative ON did not reach a nonzero percentage")
        if scenario == "toggle" and result.get("representative_off") is not True:
            errors.append("representative OFF did not reach zero percentage")
        if result.get("stopped") is not True:
            errors.append("final OFF was not confirmed")
    elif scenario == "multi_mix":
        if not isinstance(result.get("sent"), int) or result["sent"] <= 0:
            errors.append("sent must be positive")
        accounting = result.get("fan_accounting")
        rejected = result.get("client_rejected")
        stopped = result.get("stopped")
        for idx in fan_indices:
            if not isinstance(rejected, dict) or rejected.get(idx) != 0:
                errors.append(f"fan{idx} had rejected MQTT publishes")
            if not isinstance(accounting, dict) or not isinstance(accounting.get(idx), dict):
                errors.append(f"fan{idx} has no command accounting")
            else:
                errors.extend(
                    f"fan{idx}: {error}"
                    for error in _accounting_result_errors(accounting[idx], minimum_handled_ratio)
                )
            if not isinstance(stopped, dict) or stopped.get(idx) is not True:
                errors.append(f"fan{idx} OFF was not confirmed")
    elif scenario == "invalid":
        sent = result.get("sent")
        if not isinstance(sent, int) or sent <= 0:
            errors.append("no invalid commands were sent")
        if result.get("client_rejected") != 0:
            errors.append("one or more invalid-command publishes were rejected by the client")
        if result.get("confirmed") != sent:
            errors.append("not every invalid command received a liveness confirmation")
        if result.get("failures") != []:
            errors.append(f"invalid commands changed state or lost liveness: {result.get('failures')!r}")
        if result.get("stopped") is not True:
            errors.append("final OFF was not confirmed")
    elif scenario == "rmt_check":
        if result.get("missing") != []:
            errors.append(f"missing RMT metrics: {result.get('missing')!r}")
        if result.get("counter_resets") != []:
            errors.append(f"RMT counters reset: {result.get('counter_resets')!r}")
        for idx in fan_indices:
            delta = result.get(f"fan{idx}")
            if not isinstance(delta, int):
                errors.append(f"fan{idx} has no RMT delta")
            elif not 1 <= delta <= rmt_refresh_max:
                errors.append(f"fan{idx} RMT delta {delta} is outside 1..{rmt_refresh_max}")
    else:
        errors.append(f"no validator for scenario {scenario!r}")
    return errors


def _json_safe(value: object) -> object:
    if isinstance(value, float) and not math.isfinite(value):
        return None
    if isinstance(value, dict):
        return {str(key): _json_safe(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_safe(item) for item in value]
    return value


def _write_json_report(destination: str, report: Dict[str, object]) -> None:
    rendered = json.dumps(_json_safe(report), indent=2, sort_keys=True, allow_nan=False) + "\n"
    if destination == "-":
        sys.stdout.write(rendered)
        return
    path = Path(destination)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(rendered, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description="MQTT stress test for greenhouse_esp")
    parser.add_argument(
        "--broker", default=os.getenv("MQTT_BROKER"),
        help="mqtt://host:1883 or mqtts://host:8883 (or MQTT_BROKER)",
    )
    parser.add_argument("--username", default=os.getenv("MQTT_USERNAME"))
    parser.add_argument("--password", default=os.getenv("MQTT_PASSWORD"))
    parser.add_argument(
        "--root-topic", "--topic-prefix", dest="root_topic",
        default=os.getenv("MQTT_ROOT_TOPIC", os.getenv("MQTT_TOPIC_PREFIX", "greenhouse_esp")),
        help="single root MQTT level; --topic-prefix is a deprecated alias",
    )
    parser.add_argument("--node-id", default=os.getenv("MQTT_NODE_ID"))
    parser.add_argument(
        "--legacy-topics", action="store_true", default=_env_flag("MQTT_LEGACY_TOPICS"),
        help="use <root>/fanN topics; flood command accounting will be unsupported",
    )
    parser.add_argument("--seed", type=int, default=int(os.getenv("STRESS_SEED", "0")))
    parser.add_argument("--json-output", default=os.getenv("STRESS_JSON_OUTPUT"))
    parser.add_argument("--fan-start", type=int, default=int(os.getenv("FAN_INDEX_START", "1")))
    parser.add_argument("--fan-count", type=int, default=int(os.getenv("FAN_COUNT", "1")))
    parser.add_argument(
        "--scenarios", default="latency,spam,toggle,multi_mix,invalid,rmt_check",
        help="comma-separated: latency, toggle, spam, multi_mix, invalid, rmt_check",
    )
    parser.add_argument("--duration", type=int, default=20, help="duration for spam/multi_mix")
    parser.add_argument("--rate", type=int, default=30, help="publish rate for spam/multi_mix")
    parser.add_argument("--latency-first-p95", type=float, default=float(os.getenv("LAT_FIRST_P95", "1.5")))
    parser.add_argument("--latency-settle-p95", type=float, default=float(os.getenv("LAT_SETTLE_P95", "6.0")))
    parser.add_argument("--rmt-refresh-max", type=int, default=int(os.getenv("RMT_REFRESH_MAX_5S", "5")))
    parser.add_argument(
        "--minimum-handled-ratio", type=float,
        default=float(os.getenv("MINIMUM_HANDLED_RATIO", "1.0")),
    )
    parser.add_argument("--log-level", default="INFO")
    args = parser.parse_args()

    if not args.broker:
        parser.error("--broker or MQTT_BROKER is required")
    if not args.legacy_topics and not args.node_id:
        parser.error("--node-id or MQTT_NODE_ID is required unless --legacy-topics is used")
    if args.fan_start < 1 or args.fan_count < 1:
        parser.error("--fan-start and --fan-count must be at least 1")
    if args.rate < 1 or args.duration < 1:
        parser.error("--rate and --duration must be at least 1")
    if args.latency_first_p95 <= 0 or args.latency_settle_p95 <= 0:
        parser.error("latency thresholds must be greater than zero")
    if args.rmt_refresh_max < 0:
        parser.error("--rmt-refresh-max must not be negative")
    if not 0.0 <= args.minimum_handled_ratio <= 1.0:
        parser.error("--minimum-handled-ratio must be from 0 through 1")

    aliases = {"toggle_storm": "toggle"}
    requested = [value.strip() for value in args.scenarios.split(",") if value.strip()]
    if not requested:
        parser.error("--scenarios must contain at least one scenario")
    supported = {"latency", "toggle", "spam", "multi_mix", "invalid", "rmt_check"}
    scenario_plan = [(name, aliases.get(name, name)) for name in requested]
    unknown = [name for name, scenario in scenario_plan if scenario not in supported]
    if unknown:
        parser.error(f"unknown scenario(s): {', '.join(unknown)}")

    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(message)s",
    )
    fan_indices = list(range(args.fan_start, args.fan_start + args.fan_count))
    try:
        tester = StressTester(
            broker_uri=args.broker,
            username=args.username,
            password=args.password,
            topic_prefix=args.root_topic,
            node_id=args.node_id,
            legacy_topics=args.legacy_topics,
            seed=args.seed,
            fan_indices=fan_indices,
        )
    except (RuntimeError, ValueError) as exc:
        parser.error(str(exc))

    results: Dict[str, object] = {}
    validation_errors: List[str] = []
    connected = False
    shutdown: Dict[int, bool] = {}
    try:
        tester.connect()
        connected = True
        initial_stop = tester.stop_all()
        if not all(initial_stop.values()):
            raise RuntimeError(f"controller did not confirm initial OFF state: {initial_stop}")
        if not tester.wait_for_pubacks(5.0):
            raise RuntimeError("initial safety-stop publishes did not all receive PUBACK")
        logging.info("Running scenarios on fans %s with seed %d", fan_indices, args.seed)
        for _requested_name, scenario in scenario_plan:
            if scenario in {"latency", "toggle", "spam", "invalid"}:
                for idx in fan_indices:
                    if scenario == "latency":
                        result = tester.scenario_latency(idx, iterations=10)
                    elif scenario == "toggle":
                        result = tester.scenario_toggle_storm(idx, cycles=50, interval=0.05)
                    elif scenario == "spam":
                        result = tester.scenario_spam(idx, rate_hz=args.rate, duration_s=args.duration)
                    else:
                        result = tester.scenario_invalid(idx)
                    key = f"{scenario}_fan{idx}"
                    results[key] = result
                    validation_errors.extend(
                        f"{key}: {error}"
                        for error in scenario_result_errors(
                            scenario, result, [idx],
                            latency_first_p95=args.latency_first_p95,
                            latency_settle_p95=args.latency_settle_p95,
                            rmt_refresh_max=args.rmt_refresh_max,
                            minimum_handled_ratio=args.minimum_handled_ratio,
                        )
                    )
            elif scenario == "multi_mix":
                result = tester.scenario_multi_mix(rate_hz=args.rate, duration_s=args.duration)
                results[scenario] = result
                validation_errors.extend(
                    f"{scenario}: {error}"
                    for error in scenario_result_errors(
                        scenario, result, fan_indices,
                        minimum_handled_ratio=args.minimum_handled_ratio,
                    )
                )
            else:
                result = tester.scenario_rmt_check()
                results[scenario] = result
                validation_errors.extend(
                    f"{scenario}: {error}"
                    for error in scenario_result_errors(
                        scenario, result, fan_indices, rmt_refresh_max=args.rmt_refresh_max
                    )
                )
    except Exception as exc:
        logging.exception("Stress run aborted")
        validation_errors.append(f"runtime: {type(exc).__name__}: {exc}")
    finally:
        if connected:
            try:
                shutdown = tester.disconnect(stop_fans=True)
            except Exception as exc:
                logging.exception("Shutdown cleanup failed")
                validation_errors.append(f"shutdown: {type(exc).__name__}: {exc}")
    if shutdown and not all(shutdown.values()):
        validation_errors.append("shutdown: one or more fans did not confirm OFF during cleanup")

    report: Dict[str, object] = {
        "schema_version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "success": not validation_errors,
        "seed": args.seed,
        "node_id": args.node_id,
        "legacy_topics": args.legacy_topics,
        "root_topic": tester.root_topic,
        "topic_base": tester.topic_prefix,
        "broker": {"host": tester.host, "port": tester.port, "tls": tester.secure},
        "configuration": {
            "fan_indices": fan_indices,
            "scenarios": [scenario for _name, scenario in scenario_plan],
            "rate_hz": args.rate,
            "duration_s": args.duration,
            "latency_first_p95_s": args.latency_first_p95,
            "latency_settle_p95_s": args.latency_settle_p95,
            "rmt_refresh_max": args.rmt_refresh_max,
            "minimum_handled_ratio": args.minimum_handled_ratio,
        },
        "results": results,
        "validation_errors": validation_errors,
        "shutdown": shutdown,
        "publish_totals": tester.publish_stats(),
    }
    summary_stream = sys.stderr if args.json_output == "-" else sys.stdout
    print("\n==== Stress Test Summary ====", file=summary_stream)
    for name, result in results.items():
        print(f"{name}: {result}", file=summary_stream)
    print(f"shutdown: {shutdown}", file=summary_stream)
    if validation_errors:
        print("failures:", file=summary_stream)
        for error in validation_errors:
            print(f"- {error}", file=summary_stream)
    if args.json_output:
        _write_json_report(args.json_output, report)
    if validation_errors:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
