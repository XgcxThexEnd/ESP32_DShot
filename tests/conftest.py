import os
import sys
from pathlib import Path
from typing import List, Optional, Tuple

import pytest


def _env_flag(name: str, default: bool = False) -> bool:
    value = os.getenv(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


def pytest_addoption(parser: pytest.Parser) -> None:
    group = parser.getgroup("greenhouse_esp")
    group.addoption(
        "--run-live-mqtt",
        action="store_true",
        default=_env_flag("RUN_LIVE_MQTT"),
        help="run tests that command real fans through a live MQTT broker",
    )
    group.addoption(
        "--broker",
        action="store",
        default=os.getenv("MQTT_BROKER", "mqtt://127.0.0.1:1883"),
        help="MQTT broker URI, e.g. mqtt://host:1883 or mqtts://host:8883",
    )
    group.addoption("--mqtt-username", action="store", default=os.getenv("MQTT_USERNAME"))
    group.addoption("--mqtt-password", action="store", default=os.getenv("MQTT_PASSWORD"))
    group.addoption(
        "--root-topic",
        "--topic-prefix",
        dest="root_topic",
        action="store",
        default=os.getenv("MQTT_ROOT_TOPIC", os.getenv("MQTT_TOPIC_PREFIX", "greenhouse_esp")),
        help="single MQTT root level; --topic-prefix is a deprecated alias",
    )
    group.addoption("--node-id", action="store", default=os.getenv("MQTT_NODE_ID"))
    group.addoption(
        "--legacy-topics",
        action="store_true",
        default=_env_flag("MQTT_LEGACY_TOPICS"),
        help="exercise legacy <root>/fanN topics (flood accounting is unsupported)",
    )
    group.addoption(
        "--seed",
        action="store",
        type=int,
        default=int(os.getenv("STRESS_SEED", "0")),
        help="deterministic stress-workload seed",
    )
    group.addoption(
        "--fan-start",
        action="store",
        type=int,
        default=int(os.getenv("FAN_INDEX_START", "1")),
    )
    group.addoption(
        "--fan-count",
        action="store",
        type=int,
        default=int(os.getenv("FAN_COUNT", "1")),
    )
    group.addoption("--rate", action="store", type=int, default=int(os.getenv("STRESS_RATE", "30")))
    group.addoption(
        "--duration",
        action="store",
        type=int,
        default=int(os.getenv("STRESS_DURATION", "15")),
    )
    group.addoption(
        "--latency-first-p95",
        action="store",
        type=float,
        default=float(os.getenv("LAT_FIRST_P95", "1.5")),
        help="maximum acceptable p95 latency for a fresh state observation",
    )
    group.addoption(
        "--latency-settle-p95",
        action="store",
        type=float,
        default=float(os.getenv("LAT_SETTLE_P95", "6.0")),
        help="maximum acceptable p95 latency to reach the commanded target",
    )
    group.addoption(
        "--rmt-refresh-max",
        action="store",
        type=int,
        default=int(os.getenv("RMT_REFRESH_MAX_5S", "5")),
        help="maximum acceptable rmt_refresh_count increase between metric rounds",
    )
    group.addoption(
        "--minimum-handled-ratio",
        action="store",
        type=float,
        default=float(os.getenv("MINIMUM_HANDLED_RATIO", "1.0")),
        help="minimum accepted-command counter ratio for flood scenarios",
    )


def pytest_configure(config: pytest.Config) -> None:
    config.addinivalue_line(
        "markers",
        "stress: commands physical fans through a live MQTT-connected controller",
    )
    for option in ("--fan-start", "--fan-count", "--rate", "--duration"):
        if int(config.getoption(option)) < 1:
            raise pytest.UsageError(f"{option} must be at least 1")
    ratio = float(config.getoption("--minimum-handled-ratio"))
    if not 0.0 <= ratio <= 1.0:
        raise pytest.UsageError("--minimum-handled-ratio must be from 0 through 1")
    if (
        config.getoption("--run-live-mqtt")
        and not config.getoption("--legacy-topics")
        and not config.getoption("--node-id")
    ):
        raise pytest.UsageError(
            "--node-id or MQTT_NODE_ID is required for node-scoped live MQTT tests"
        )


def pytest_collection_modifyitems(config: pytest.Config, items: List[pytest.Item]) -> None:
    if config.getoption("--run-live-mqtt"):
        return
    skip = pytest.mark.skip(
        reason="live actuator test; pass --run-live-mqtt on a mechanically safe rig",
    )
    for item in items:
        if "stress" in item.keywords:
            item.add_marker(skip)


@pytest.fixture(scope="session")
def broker(request: pytest.FixtureRequest) -> str:
    return str(request.config.getoption("--broker"))


@pytest.fixture(scope="session")
def mqtt_credentials(request: pytest.FixtureRequest) -> Tuple[Optional[str], Optional[str]]:
    return (
        request.config.getoption("--mqtt-username"),
        request.config.getoption("--mqtt-password"),
    )


@pytest.fixture(scope="session")
def topic_prefix(request: pytest.FixtureRequest) -> str:
    return str(request.config.getoption("--root-topic")).strip("/")


@pytest.fixture(scope="session")
def node_id(request: pytest.FixtureRequest) -> Optional[str]:
    value = request.config.getoption("--node-id")
    return str(value) if value else None


@pytest.fixture(scope="session")
def legacy_topics(request: pytest.FixtureRequest) -> bool:
    return bool(request.config.getoption("--legacy-topics"))


@pytest.fixture(scope="session")
def seed(request: pytest.FixtureRequest) -> int:
    return int(request.config.getoption("--seed"))


@pytest.fixture(scope="session")
def fan_indices(request: pytest.FixtureRequest) -> List[int]:
    start = int(request.config.getoption("--fan-start"))
    count = int(request.config.getoption("--fan-count"))
    if start < 1:
        pytest.fail("--fan-start must be at least 1")
    if count < 1:
        pytest.fail("--fan-count must be at least 1")
    return list(range(start, start + count))


@pytest.fixture(scope="session")
def rate(request: pytest.FixtureRequest) -> int:
    value = int(request.config.getoption("--rate"))
    if value < 1:
        pytest.fail("--rate must be at least 1")
    return value


@pytest.fixture(scope="session")
def duration(request: pytest.FixtureRequest) -> int:
    value = int(request.config.getoption("--duration"))
    if value < 1:
        pytest.fail("--duration must be at least 1")
    return value


@pytest.fixture(scope="session")
def thresholds(request: pytest.FixtureRequest) -> dict:
    return {
        "lat_first_p95": float(request.config.getoption("--latency-first-p95")),
        "lat_settle_p95": float(request.config.getoption("--latency-settle-p95")),
        "rmt_refresh_max": int(request.config.getoption("--rmt-refresh-max")),
        "minimum_handled_ratio": float(request.config.getoption("--minimum-handled-ratio")),
    }


@pytest.fixture(scope="session")
def tester(
    broker: str,
    mqtt_credentials,
    topic_prefix: str,
    node_id: Optional[str],
    legacy_topics: bool,
    seed: int,
    fan_indices: List[int],
):
    repo_root = Path(__file__).resolve().parents[1]
    tools_dir = repo_root / "tools"
    sys.path.insert(0, str(tools_dir))
    from stress_test import StressTester

    username, password = mqtt_credentials
    instance = StressTester(
        broker_uri=broker,
        username=username,
        password=password,
        topic_prefix=topic_prefix,
        node_id=node_id,
        legacy_topics=legacy_topics,
        seed=seed,
        fan_indices=fan_indices,
    )
    instance.connect()

    try:
        initial_stop = instance.stop_all()
    except Exception:
        instance.disconnect(stop_fans=False)
        raise
    if not all(initial_stop.values()):
        instance.disconnect(stop_fans=False)
        pytest.fail(f"controller did not provide fresh OFF confirmations: {initial_stop}")
    if not instance.wait_for_pubacks(5.0):
        instance.disconnect(stop_fans=False)
        pytest.fail("initial safety-stop publishes did not all receive PUBACK")

    try:
        yield instance
    finally:
        shutdown = instance.disconnect(stop_fans=True)
        missing = [idx for idx, confirmed in shutdown.items() if not confirmed]
        if missing:
            pytest.fail(f"could not confirm OFF cleanup for fans {missing}")
