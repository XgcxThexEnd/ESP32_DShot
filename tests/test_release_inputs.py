import json
from pathlib import Path

from scripts.check_release_inputs import check_inputs

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = json.loads((ROOT / "configs/release-target.json").read_text())
WORKFLOW = (ROOT / ".github/workflows/ci.yml").read_text()
LOCK = """dependencies:
  idf:
    source:
      type: idf
    version: 6.1.0
target: esp32s3
version: 3.0.0
"""


def test_matching_release_inputs():
    assert check_inputs(CONTRACT, LOCK, WORKFLOW) == []


def test_local_sdk_cannot_silently_replace_release_lock():
    errors = check_inputs(CONTRACT, LOCK.replace("6.1.0", "5.5.0"), WORKFLOW)
    assert any("ESP-IDF version" in error for error in errors)


def test_wrong_chip_and_schema_are_rejected():
    errors = check_inputs(CONTRACT, LOCK.replace("esp32s3", "esp32").replace("3.0.0", "2.0.0"), WORKFLOW)
    assert len(errors) == 2


def test_builder_digest_and_actual_ci_target_are_checked():
    drift = WORKFLOW.replace(CONTRACT["builder_image"], "espressif/idf:latest", 1)
    drift = drift.replace("set-target esp32s3", "set-target esp32")
    assert len(check_inputs(CONTRACT, LOCK, drift)) == 2
