#!/usr/bin/env python3
"""Validate advisory gardener messages without disclosing payload values."""

from __future__ import annotations

import argparse
import json
import math
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path, PurePosixPath
from typing import Any

from jsonschema import Draft202012Validator, FormatChecker


ROOT = Path(__file__).resolve().parents[1]
SCHEMAS = {
    "observation": ROOT / "schemas" / "gardener-observation.schema.json",
    "recommendation": ROOT / "schemas" / "gardener-recommendation.schema.json",
}
MAX_RECOMMENDATION_TTL_SECONDS = 3600
MAX_OBSERVATION_AGE_SECONDS = 300
MAX_CLOCK_SKEW_SECONDS = 60


class MessageDocumentError(ValueError):
    """The input used a JSON representation which is unsafe to validate."""


def _reject_duplicate_members(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise MessageDocumentError("JSON object contains a duplicate member")
        result[key] = value
    return result


def _reject_non_json_number(_: str) -> Any:
    raise MessageDocumentError("JSON contains a non-finite numeric constant")


def load_message_json(text: str) -> Any:
    """Parse strict JSON, rejecting duplicate members and NaN/Infinity."""

    return json.loads(
        text,
        object_pairs_hook=_reject_duplicate_members,
        parse_constant=_reject_non_json_number,
    )


def _timestamp(value: Any) -> datetime | None:
    if not isinstance(value, str):
        return None
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None
    return parsed if parsed.tzinfo is not None and parsed.utcoffset() is not None else None


def _location(parts: Any) -> str:
    path = ".".join(str(part) for part in parts)
    return path or "message"


def _contains_non_finite_number(value: Any) -> bool:
    if isinstance(value, float):
        return not math.isfinite(value)
    if isinstance(value, dict):
        return any(_contains_non_finite_number(item) for item in value.values())
    if isinstance(value, list):
        return any(_contains_non_finite_number(item) for item in value)
    return False


def _current_time(now: datetime | None) -> datetime:
    current = now or datetime.now(timezone.utc)
    if current.tzinfo is None or current.utcoffset() is None:
        raise ValueError("now must be timezone-aware")
    return current


def validate_message(
    kind: str,
    message: Any,
    *,
    require_current: bool = False,
    now: datetime | None = None,
) -> list[str]:
    """Return value-redacting schema and semantic validation failures."""

    if kind not in SCHEMAS:
        raise ValueError("kind must be observation or recommendation")
    schema = json.loads(SCHEMAS[kind].read_text(encoding="utf-8"))
    Draft202012Validator.check_schema(schema)
    validator = Draft202012Validator(schema, format_checker=FormatChecker())
    errors = [
        f"{_location(error.absolute_path)} violates {error.validator}"
        for error in validator.iter_errors(message)
    ]
    if _contains_non_finite_number(message):
        errors.append("message contains a non-finite number")
    if not isinstance(message, dict):
        return sorted(set(errors))

    if kind == "observation":
        captured = _timestamp(message.get("captured_at"))
        processed = _timestamp(message.get("processed_at"))
        if captured is None:
            errors.append("captured_at is not a timezone-aware timestamp")
        if processed is None:
            errors.append("processed_at is not a timezone-aware timestamp")
        if captured is not None and processed is not None and processed < captured:
            errors.append("processed_at precedes captured_at")

        subject = message.get("subject")
        roi = subject.get("roi") if isinstance(subject, dict) else None
        if isinstance(roi, dict):
            x, y = roi.get("x"), roi.get("y")
            width, height = roi.get("width"), roi.get("height")
            if all(
                isinstance(value, (int, float)) and not isinstance(value, bool)
                for value in (x, y, width, height)
            ):
                if x + width > 1:
                    errors.append("subject.roi extends past the image width")
                if y + height > 1:
                    errors.append("subject.roi extends past the image height")

        image = message.get("image")
        object_key = image.get("object_key") if isinstance(image, dict) else None
        if isinstance(object_key, str):
            key = PurePosixPath(object_key)
            raw_parts = object_key.split("/")
            if (
                key.is_absolute()
                or any(part in {"", ".", ".."} for part in raw_parts)
                or key.as_posix() != object_key
            ):
                errors.append("image.object_key is not a safe opaque object key")

        calibration = message.get("calibration")
        quality = message.get("quality")
        if (
            isinstance(calibration, dict)
            and calibration.get("reference_visible") is False
            and isinstance(quality, dict)
            and quality.get("status") == "ok"
        ):
            errors.append(
                "quality.status cannot be ok when the calibration reference is not visible"
            )

        camera_id = message.get("camera_id")
        metrics = message.get("metrics")
        if isinstance(camera_id, str) and isinstance(metrics, list):
            for index, metric in enumerate(metrics):
                if (
                    isinstance(metric, dict)
                    and metric.get("source") == "camera"
                    and metric.get("source_id") != camera_id
                ):
                    errors.append(
                        f"metrics.{index}.source_id does not match camera_id"
                    )

        if require_current and captured is not None and processed is not None:
            current = _current_time(now)
            future_limit = current + timedelta(seconds=MAX_CLOCK_SKEW_SECONDS)
            if captured > future_limit or processed > future_limit:
                errors.append("observation timestamp exceeds the allowed clock skew")
            if (current - captured).total_seconds() > MAX_OBSERVATION_AGE_SECONDS:
                errors.append(
                    f"observation is older than {MAX_OBSERVATION_AGE_SECONDS} seconds"
                )
    else:
        created = _timestamp(message.get("created_at"))
        expires = _timestamp(message.get("expires_at"))
        if created is None:
            errors.append("created_at is not a timezone-aware timestamp")
        if expires is None:
            errors.append("expires_at is not a timezone-aware timestamp")
        if created is not None and expires is not None:
            ttl = (expires - created).total_seconds()
            if ttl <= 0:
                errors.append("expires_at must be later than created_at")
            elif ttl > MAX_RECOMMENDATION_TTL_SECONDS:
                errors.append(
                    f"recommendation lifetime exceeds {MAX_RECOMMENDATION_TTL_SECONDS} seconds"
                )
            if require_current:
                current = _current_time(now)
                if created > current + timedelta(seconds=MAX_CLOCK_SKEW_SECONDS):
                    errors.append(
                        "recommendation creation time exceeds the allowed clock skew"
                    )
                if expires <= current:
                    errors.append("recommendation has expired")

    return sorted(set(errors))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=tuple(SCHEMAS))
    parser.add_argument("message", type=Path)
    parser.add_argument(
        "--require-current",
        action="store_true",
        help="also reject an already-expired recommendation",
    )
    args = parser.parse_args(argv)

    try:
        message = load_message_json(args.message.read_text(encoding="utf-8"))
        errors = validate_message(
            args.kind, message, require_current=args.require_current
        )
    except (OSError, UnicodeError, json.JSONDecodeError, MessageDocumentError) as exc:
        print(
            f"Gardener message validation could not read JSON ({exc.__class__.__name__}).",
            file=sys.stderr,
        )
        return 2

    if errors:
        print("Gardener message validation failed; values are intentionally hidden:")
        for error in errors:
            print(f"- {error}")
        return 1
    print(f"Gardener {args.kind} message passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
