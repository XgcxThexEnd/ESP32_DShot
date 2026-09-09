import json
import math
import sys
from datetime import datetime, timezone
from pathlib import Path

import pytest
from jsonschema import Draft202012Validator


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from validate_gardener_message import (  # noqa: E402
    MessageDocumentError,
    load_message_json,
    validate_message,
)


def load_schema(name: str) -> dict:
    return json.loads((ROOT / "schemas" / name).read_text(encoding="utf-8"))


def test_observation_contract_requires_traceable_evidence() -> None:
    schema = load_schema("gardener-observation.schema.json")
    Draft202012Validator.check_schema(schema)
    required = set(schema["required"])
    assert {
        "observation_id",
        "captured_at",
        "processed_at",
        "camera_id",
        "subject",
        "calibration",
        "model",
        "image",
        "quality",
    } <= required
    assert schema["properties"]["image"]["properties"]["sha256"]["pattern"] == "^[a-f0-9]{64}$"
    assert schema["properties"]["quality"]["properties"]["score"]["maximum"] == 1


def test_recommendation_contract_is_advisory_and_expires() -> None:
    schema = load_schema("gardener-recommendation.schema.json")
    Draft202012Validator.check_schema(schema)
    required = set(schema["required"])
    assert {"recommendation_id", "created_at", "expires_at", "confidence"} <= required
    assert {"evidence_observation_ids", "requires_human_approval", "actuation_allowed"} <= required
    assert schema["properties"]["requires_human_approval"]["const"] is True
    assert schema["properties"]["actuation_allowed"]["const"] is False
    assert schema["properties"]["suggested_fan_percentage"]["maximum"] == 100


def test_gardener_design_denies_direct_actuator_authority() -> None:
    design = (ROOT / "docs" / "ai-gardener.md").read_text(encoding="utf-8")
    assert "must not have broker permission" in design
    assert "can never publish OTA" in design
    assert "actuation_allowed: false" in design


def valid_observation() -> dict:
    return {
        "schema_version": 1,
        "observation_id": "obs_001",
        "captured_at": "2026-09-06T12:00:00Z",
        "processed_at": "2026-09-06T12:00:04Z",
        "zone_id": "north_bed",
        "camera_id": "camera_1",
        "subject": {
            "subject_id": "plant_12",
            "roi": {"x": 0.1, "y": 0.2, "width": 0.4, "height": 0.5},
        },
        "calibration": {
            "calibration_id": "north_bed_v1",
            "version": "1",
            "reference_visible": True,
        },
        "model": {"name": "canopy-metrics", "version": "1.0.0"},
        "image": {"object_key": "north_bed/2026/09/obs_001.jpg", "sha256": "a" * 64},
        "quality": {"status": "ok", "score": 0.97},
        "metrics": [
            {
                "name": "canopy_coverage",
                "value": 0.72,
                "unit": "ratio",
                "confidence": 0.91,
                "source": "camera",
                "source_id": "camera_1",
            }
        ],
    }


def valid_recommendation() -> dict:
    return {
        "schema_version": 1,
        "recommendation_id": "rec_001",
        "created_at": "2026-09-06T12:00:05Z",
        "expires_at": "2026-09-06T12:15:05Z",
        "zone_id": "north_bed",
        "model": {"name": "gardener-advisor", "version": "1.0.0"},
        "kind": "ventilation_review",
        "confidence": 0.82,
        "evidence_observation_ids": ["obs_001"],
        "rationale": "Canopy temperature trend warrants operator review.",
        "suggested_fan_percentage": 35,
        "suggested_duration_seconds": 900,
        "requires_human_approval": True,
        "actuation_allowed": False,
    }


def test_valid_gardener_messages_pass_executable_contracts() -> None:
    assert validate_message("observation", valid_observation()) == []
    assert validate_message("recommendation", valid_recommendation()) == []


def test_observation_rejects_url_key_bad_roi_and_time_order() -> None:
    observation = valid_observation()
    observation["image"]["object_key"] = "https://user:secret@example.invalid/image.jpg"
    observation["subject"]["roi"] = {
        "x": 0.8,
        "y": 0.8,
        "width": 0.3,
        "height": 0.3,
    }
    observation["processed_at"] = "2026-09-06T11:59:59Z"

    errors = validate_message("observation", observation)
    output = "\n".join(errors)

    assert "object_key" in output
    assert "image width" in output
    assert "image height" in output
    assert "precedes captured_at" in output
    assert "user:secret" not in output


def test_recommendation_rejects_unbounded_or_direct_actuation() -> None:
    recommendation = valid_recommendation()
    recommendation["expires_at"] = "2026-09-06T14:00:05Z"
    recommendation["requires_human_approval"] = False
    recommendation["actuation_allowed"] = True

    errors = validate_message("recommendation", recommendation)
    output = "\n".join(errors)

    assert "lifetime exceeds" in output
    assert "requires_human_approval" in output
    assert "actuation_allowed" in output


def test_strict_json_and_library_validation_reject_numeric_bypasses() -> None:
    with pytest.raises(MessageDocumentError):
        load_message_json('{"confidence": 0.5, "confidence": 0.9}')
    with pytest.raises(MessageDocumentError):
        load_message_json('{"confidence": NaN}')

    observation = valid_observation()
    observation["quality"]["score"] = math.nan
    recommendation = valid_recommendation()
    recommendation["confidence"] = math.inf

    assert "non-finite" in "\n".join(validate_message("observation", observation))
    assert "non-finite" in "\n".join(
        validate_message("recommendation", recommendation)
    )


def test_current_gate_rejects_stale_observations_and_future_advice() -> None:
    current = datetime(2026, 9, 6, 12, 10, tzinfo=timezone.utc)
    observation = valid_observation()
    observation_errors = validate_message(
        "observation", observation, require_current=True, now=current
    )
    assert any("older than 300 seconds" in error for error in observation_errors)

    recommendation = valid_recommendation()
    recommendation["created_at"] = "2099-01-01T00:00:00Z"
    recommendation["expires_at"] = "2099-01-01T00:30:00Z"
    recommendation_errors = validate_message(
        "recommendation", recommendation, require_current=True, now=current
    )
    assert any("allowed clock skew" in error for error in recommendation_errors)


def test_observation_rejects_ambiguous_keys_and_inconsistent_camera_quality() -> None:
    for object_key in ("frames//image.jpg", "frames/./image.jpg", "frames/image.jpg/"):
        observation = valid_observation()
        observation["image"]["object_key"] = object_key
        assert any(
            "safe opaque object key" in error
            for error in validate_message("observation", observation)
        )

    observation = valid_observation()
    observation["calibration"]["reference_visible"] = False
    observation["metrics"][0]["source_id"] = "different_camera"
    errors = validate_message("observation", observation)
    assert any("calibration reference" in error for error in errors)
    assert any("does not match camera_id" in error for error in errors)
