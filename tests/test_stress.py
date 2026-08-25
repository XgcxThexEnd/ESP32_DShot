import sys
from pathlib import Path
from typing import List

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from stress_test import scenario_result_errors  # noqa: E402


def assert_valid(scenario: str, result: dict, fans: List[int], thresholds: dict) -> None:
    errors = scenario_result_errors(
        scenario,
        result,
        fans,
        latency_first_p95=thresholds["lat_first_p95"],
        latency_settle_p95=thresholds["lat_settle_p95"],
        rmt_refresh_max=thresholds["rmt_refresh_max"],
        minimum_handled_ratio=thresholds["minimum_handled_ratio"],
    )
    assert errors == [], {"errors": errors, "result": result}


@pytest.mark.stress
def test_latency(tester, fan_indices: List[int], thresholds: dict) -> None:
    for idx in fan_indices:
        assert_valid("latency", tester.scenario_latency(idx, iterations=10), [idx], thresholds)


@pytest.mark.stress
def test_toggle_storm(tester, fan_indices: List[int], thresholds: dict) -> None:
    for idx in fan_indices:
        result = tester.scenario_toggle_storm(idx, cycles=50, interval=0.05)
        assert result["flips"] == 100, result
        assert_valid("toggle", result, [idx], thresholds)


@pytest.mark.stress
def test_spam(
    tester, fan_indices: List[int], rate: int, duration: int, thresholds: dict
) -> None:
    for idx in fan_indices:
        result = tester.scenario_spam(idx, rate_hz=rate, duration_s=duration)
        assert result["sent"] >= int(0.8 * rate * duration), result
        assert_valid("spam", result, [idx], thresholds)


@pytest.mark.stress
def test_multi_mix(
    tester, fan_indices: List[int], rate: int, duration: int, thresholds: dict
) -> None:
    result = tester.scenario_multi_mix(rate_hz=rate, duration_s=duration)
    assert result["sent"] >= int(0.8 * rate * duration * len(fan_indices)), result
    assert_valid("multi_mix", result, fan_indices, thresholds)


@pytest.mark.stress
def test_invalid_payloads_are_rejected(
    tester, fan_indices: List[int], thresholds: dict
) -> None:
    for idx in fan_indices:
        assert_valid("invalid", tester.scenario_invalid(idx), [idx], thresholds)


@pytest.mark.stress
def test_rmt_refresh_health(tester, fan_indices: List[int], thresholds: dict) -> None:
    assert_valid("rmt_check", tester.scenario_rmt_check(), fan_indices, thresholds)
