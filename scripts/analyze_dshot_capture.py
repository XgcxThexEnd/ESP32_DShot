#!/usr/bin/env python3
"""Validate DShot600 edge-transition captures exported as two-column CSV.

The first column is a timestamp and the second is the logic level *after* the
transition.  A single header row is optional.  This module intentionally uses
only the Python standard library so it can run on an isolated HIL workstation.
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional, Sequence, Tuple


DSHOT600_BIT_PERIOD_US = 1_000_000.0 / 600_000.0
DSHOT600_ZERO_HIGH_US = DSHOT600_BIT_PERIOD_US * 0.375
DSHOT600_ONE_HIGH_US = DSHOT600_BIT_PERIOD_US * 0.750
DEFAULT_TIMING_TOLERANCE_PCT = 20.0
DEFAULT_MINIMUM_FRAME_GAP_US = 2.0
DEFAULT_MAX_EDGES = 1_000_000

TIME_UNIT_TO_US = {
    "s": 1_000_000.0,
    "ms": 1_000.0,
    "us": 1.0,
    "ns": 0.001,
}


class CaptureError(ValueError):
    """Raised when a capture cannot prove a valid DShot600 stream."""


@dataclass(frozen=True)
class Transition:
    timestamp_us: float
    level: int


@dataclass(frozen=True)
class Pulse:
    rise_us: float
    fall_us: float

    @property
    def high_us(self) -> float:
        return self.fall_us - self.rise_us


@dataclass(frozen=True)
class DShotFrame:
    start_us: float
    end_us: float
    bits: Tuple[int, ...]
    word: int
    value: int
    telemetry: bool
    checksum: int
    high_widths_us: Tuple[float, ...]
    bit_periods_us: Tuple[float, ...]


@dataclass(frozen=True)
class CaptureAnalysis:
    frames: Tuple[DShotFrame, ...]
    inter_frame_low_us: Tuple[float, ...]


@dataclass(frozen=True)
class ArmingResult:
    duration_ms: float
    zero_frames: int
    max_frame_start_gap_us: float


def _parse_level(value: str) -> int:
    normalized = value.strip().lower()
    if normalized in {"0", "low", "false"}:
        return 0
    if normalized in {"1", "high", "true"}:
        return 1
    raise CaptureError(f"invalid logic level {value!r}; expected 0/1 or low/high")


def read_transitions(
    path: Path,
    time_unit: str = "us",
    max_edges: int = DEFAULT_MAX_EDGES,
) -> List[Transition]:
    """Read a two-column edge CSV and normalize timestamps to microseconds."""

    if time_unit not in TIME_UNIT_TO_US:
        raise CaptureError(f"unsupported timestamp unit {time_unit!r}")
    if max_edges < 1:
        raise CaptureError("maximum edge count must be at least one")

    transitions: List[Transition] = []
    header_consumed = False
    scale = TIME_UNIT_TO_US[time_unit]

    try:
        capture_file = path.open("r", encoding="utf-8-sig", newline="")
    except OSError as exc:
        raise CaptureError(f"cannot open {path}: {exc}") from exc

    with capture_file:
        for line_number, row in enumerate(csv.reader(capture_file), start=1):
            if not row or all(not field.strip() for field in row):
                continue
            if row[0].lstrip().startswith("#"):
                continue
            if len(row) != 2:
                raise CaptureError(
                    f"line {line_number}: expected exactly two CSV columns, got {len(row)}"
                )

            timestamp_error: Optional[ValueError] = None
            level_error: Optional[CaptureError] = None
            try:
                raw_timestamp = float(row[0].strip())
            except ValueError as exc:
                timestamp_error = exc
                raw_timestamp = 0.0
            try:
                level = _parse_level(row[1])
            except CaptureError as exc:
                level_error = exc
                level = 0

            # A header has two labels, not a partly malformed data record.  In
            # particular, never silently discard "bad-time,1" or "0,bad-level"
            # at the start of a safety-validation capture.
            if timestamp_error is not None or level_error is not None:
                if (
                    not transitions
                    and not header_consumed
                    and timestamp_error is not None
                    and level_error is not None
                ):
                    header_consumed = True
                    continue
                if timestamp_error is not None:
                    raise CaptureError(
                        f"line {line_number}: invalid timestamp {row[0]!r}"
                    ) from timestamp_error
                assert level_error is not None
                raise CaptureError(f"line {line_number}: {level_error}") from level_error

            timestamp_us = raw_timestamp * scale
            if not math.isfinite(timestamp_us):
                raise CaptureError(f"line {line_number}: timestamp must be finite")
            if transitions and timestamp_us <= transitions[-1].timestamp_us:
                raise CaptureError(
                    f"line {line_number}: timestamps must be strictly increasing"
                )
            transitions.append(Transition(timestamp_us, level))
            if len(transitions) > max_edges:
                raise CaptureError(
                    f"capture exceeds the configured {max_edges} edge limit; "
                    "shorten the acquisition or raise --max-edges deliberately"
                )

    if not transitions:
        raise CaptureError("capture contains no transitions")
    return transitions


def _extract_pulses(transitions: Sequence[Transition]) -> List[Pulse]:
    pulses: List[Pulse] = []
    active_rise: Optional[float] = None
    previous_timestamp: Optional[float] = None

    for index, transition in enumerate(transitions):
        if not math.isfinite(transition.timestamp_us):
            raise CaptureError(f"edge {index + 1}: timestamp must be finite")
        if previous_timestamp is not None and transition.timestamp_us <= previous_timestamp:
            raise CaptureError(f"edge {index + 1}: timestamps must be strictly increasing")
        previous_timestamp = transition.timestamp_us
        if transition.level not in (0, 1):
            raise CaptureError(f"edge {index + 1}: logic level must be 0 or 1")

        if transition.level == 1:
            if active_rise is not None:
                raise CaptureError(
                    f"edge {index + 1}: HIGH follows HIGH; capture is not an edge-transition stream"
                )
            active_rise = transition.timestamp_us
            continue

        if active_rise is None:
            if index == 0:
                # Logic-analyzer exports commonly include one initial LOW sample.
                continue
            raise CaptureError(
                f"edge {index + 1}: LOW has no preceding rising edge (wire order/glitch error)"
            )
        pulses.append(Pulse(active_rise, transition.timestamp_us))
        active_rise = None

    if active_rise is not None:
        raise CaptureError("capture ends HIGH with an incomplete pulse")
    if not pulses:
        raise CaptureError("capture contains no complete HIGH pulses")
    return pulses


def _within_tolerance(actual: float, expected: float, tolerance_fraction: float) -> bool:
    return abs(actual - expected) <= expected * tolerance_fraction


def dshot_checksum(data: int) -> int:
    """Return the four-bit DShot checksum for the 12-bit payload."""

    return (data ^ (data >> 4) ^ (data >> 8)) & 0x0F


def _word_from_bits(bits: Iterable[int]) -> int:
    word = 0
    for bit in bits:
        word = (word << 1) | bit
    return word


def _has_valid_checksum(word: int) -> bool:
    return dshot_checksum(word >> 4) == (word & 0x0F)


def _reverse_word(word: int) -> int:
    reversed_word = 0
    for _ in range(16):
        reversed_word = (reversed_word << 1) | (word & 1)
        word >>= 1
    return reversed_word


def analyze_transitions(
    transitions: Sequence[Transition],
    timing_tolerance_pct: float = DEFAULT_TIMING_TOLERANCE_PCT,
    minimum_frame_gap_us: float = DEFAULT_MINIMUM_FRAME_GAP_US,
) -> CaptureAnalysis:
    """Decode and strictly validate all complete DShot600 frames in a capture."""

    if not 0.0 < timing_tolerance_pct <= 30.0:
        raise CaptureError("timing tolerance must be greater than 0 and at most 30 percent")
    if minimum_frame_gap_us < 0.0 or not math.isfinite(minimum_frame_gap_us):
        raise CaptureError("minimum frame gap must be a finite non-negative value")

    tolerance = timing_tolerance_pct / 100.0
    period_min = DSHOT600_BIT_PERIOD_US * (1.0 - tolerance)
    period_max = DSHOT600_BIT_PERIOD_US * (1.0 + tolerance)
    pulses = _extract_pulses(transitions)

    pulse_groups: List[List[Pulse]] = [[pulses[0]]]
    for pulse in pulses[1:]:
        previous = pulse_groups[-1][-1]
        start_delta = pulse.rise_us - previous.rise_us
        if start_delta < period_min:
            raise CaptureError(
                f"pulse spacing {start_delta:.6f} us is below the DShot600 timing window"
            )
        if start_delta <= period_max:
            pulse_groups[-1].append(pulse)
        else:
            low_gap = pulse.rise_us - previous.fall_us
            if low_gap < minimum_frame_gap_us:
                raise CaptureError(
                    f"inter-frame LOW gap {low_gap:.6f} us is below "
                    f"the required {minimum_frame_gap_us:.6f} us"
                )
            pulse_groups.append([pulse])

    frames: List[DShotFrame] = []
    inter_frame_low: List[float] = []
    for frame_index, group in enumerate(pulse_groups, start=1):
        if len(group) != 16:
            raise CaptureError(
                f"frame {frame_index}: expected 16 pulses, found {len(group)} "
                "(partial frame, missing edge, or timing error)"
            )

        bits: List[int] = []
        periods: List[float] = []
        for bit_index, pulse in enumerate(group, start=1):
            width = pulse.high_us
            is_zero = _within_tolerance(width, DSHOT600_ZERO_HIGH_US, tolerance)
            is_one = _within_tolerance(width, DSHOT600_ONE_HIGH_US, tolerance)
            if is_zero == is_one:
                raise CaptureError(
                    f"frame {frame_index} bit {bit_index}: HIGH width {width:.6f} us "
                    "does not identify exactly one DShot600 bit"
                )
            bits.append(1 if is_one else 0)

            if bit_index > 1:
                period = pulse.rise_us - group[bit_index - 2].rise_us
                if not period_min <= period <= period_max:
                    raise CaptureError(
                        f"frame {frame_index} bit {bit_index}: period {period:.6f} us "
                        "is outside the DShot600 timing window"
                    )
                periods.append(period)

        bits_tuple = tuple(bits)
        word = _word_from_bits(bits_tuple)
        if not _has_valid_checksum(word):
            raise CaptureError(
                f"frame {frame_index}: checksum mismatch (received 0x{word & 0x0F:X}, "
                f"expected 0x{dshot_checksum(word >> 4):X})"
            )

        value = (word >> 5) & 0x07FF
        if 1 <= value <= 47:
            raise CaptureError(
                f"frame {frame_index}: value {value} is a reserved DShot command; "
                "this safety validator accepts only stop (0) or throttle values 48..2047"
            )

        frame = DShotFrame(
            start_us=group[0].rise_us,
            end_us=group[-1].fall_us,
            bits=bits_tuple,
            word=word,
            value=value,
            telemetry=bool((word >> 4) & 1),
            checksum=word & 0x0F,
            high_widths_us=tuple(pulse.high_us for pulse in group),
            bit_periods_us=tuple(periods),
        )
        if frames:
            inter_frame_low.append(frame.start_us - frames[-1].end_us)
        frames.append(frame)

    return CaptureAnalysis(tuple(frames), tuple(inter_frame_low))


def verify_expected_stream(
    frames: Sequence[DShotFrame],
    expected_value: int,
    expected_telemetry: Optional[bool] = None,
) -> None:
    """Compare every frame with a known vector to prove on-wire bit order.

    DShot's XOR checksum is invariant under reversing a complete 16-bit word.
    A known asymmetric test vector is therefore required to distinguish an
    MSB-first stream from a bit-reversed stream conclusively.
    """

    if expected_value < 0 or expected_value > 2047 or 1 <= expected_value <= 47:
        raise CaptureError("expected value must be stop (0) or throttle 48..2047")
    if not frames:
        raise CaptureError("cannot verify an expected stream without decoded frames")
    for index, frame in enumerate(frames, start=1):
        if frame.value != expected_value:
            raise CaptureError(
                f"frame {index}: decoded value {frame.value}, expected {expected_value}; "
                "the capture may have the wrong wire order or command"
            )
        if expected_telemetry is not None and frame.telemetry != expected_telemetry:
            raise CaptureError(
                f"frame {index}: telemetry bit is {int(frame.telemetry)}, "
                f"expected {int(expected_telemetry)}"
            )

        reversed_word = _reverse_word(frame.word)
        reversed_value = (reversed_word >> 5) & 0x07FF
        reversed_telemetry = bool((reversed_word >> 4) & 1)
        reversed_matches = (
            _has_valid_checksum(reversed_word)
            and reversed_value == expected_value
            and (
                expected_telemetry is None
                or reversed_telemetry == expected_telemetry
            )
        )
        if reversed_matches:
            raise CaptureError(
                f"frame {index}: expected vector is bit-order ambiguous; "
                "choose a nonzero asymmetric value (and constrain telemetry when needed)"
            )


def verify_zero_throttle_arming(
    frames: Sequence[DShotFrame],
    duration_ms: float,
    max_frame_start_gap_us: Optional[float] = None,
) -> ArmingResult:
    """Prove that decoded frames remain zero through a boot-arming interval."""

    if duration_ms <= 0.0 or not math.isfinite(duration_ms):
        raise CaptureError("zero-throttle arming duration must be a finite positive value")
    if max_frame_start_gap_us is not None and (
        max_frame_start_gap_us <= 0.0 or not math.isfinite(max_frame_start_gap_us)
    ):
        raise CaptureError("maximum arming frame gap must be a finite positive value")
    if not frames:
        raise CaptureError("cannot verify arming without decoded frames")

    start_us = frames[0].start_us
    deadline_us = start_us + duration_ms * 1000.0
    coverage_index: Optional[int] = None
    for index, frame in enumerate(frames):
        if frame.start_us >= deadline_us:
            coverage_index = index
            break
    if coverage_index is None:
        observed_ms = (frames[-1].start_us - start_us) / 1000.0
        raise CaptureError(
            f"capture covers only {observed_ms:.3f} ms of frame starts; "
            f"{duration_ms:.3f} ms is required"
        )

    arming_frames = frames[:coverage_index]
    for index, frame in enumerate(arming_frames, start=1):
        if frame.value != 0:
            offset_ms = (frame.start_us - start_us) / 1000.0
            raise CaptureError(
                f"arming frame {index} at +{offset_ms:.3f} ms has nonzero value {frame.value}"
            )

    relevant = frames[: coverage_index + 1]
    gaps = [
        current.start_us - previous.start_us
        for previous, current in zip(relevant, relevant[1:])
    ]
    observed_max_gap = max(gaps, default=0.0)
    if max_frame_start_gap_us is not None and observed_max_gap > max_frame_start_gap_us:
        raise CaptureError(
            f"arming frame-start gap {observed_max_gap:.6f} us exceeds "
            f"the configured {max_frame_start_gap_us:.6f} us maximum"
        )

    return ArmingResult(duration_ms, len(arming_frames), observed_max_gap)


def _format_report(
    analysis: CaptureAnalysis,
    arming: Optional[ArmingResult],
    expected_value: Optional[int],
) -> str:
    frames = analysis.frames
    value_counts = Counter((frame.value, int(frame.telemetry)) for frame in frames)
    values = ", ".join(
        f"{value}{'T' if telemetry else ''} x{count}"
        for (value, telemetry), count in sorted(value_counts.items())
    )
    widths = [width for frame in frames for width in frame.high_widths_us]
    periods = [period for frame in frames for period in frame.bit_periods_us]
    lines = [
        f"PASS: {len(frames)} DShot600 frame(s)",
        "checksum=valid reserved_values=none",
        f"values: {values}",
        f"HIGH widths: {min(widths):.6f}..{max(widths):.6f} us",
        f"bit periods: {min(periods):.6f}..{max(periods):.6f} us",
    ]
    if analysis.inter_frame_low_us:
        lines.append(
            "inter-frame LOW: "
            f"{min(analysis.inter_frame_low_us):.6f}.."
            f"{max(analysis.inter_frame_low_us):.6f} us"
        )
    if expected_value is None:
        lines.append(
            "wire order: interpreted MSB-first; use --expect-value with a known "
            "asymmetric command for a conclusive check"
        )
    else:
        lines.append(
            f"wire order: MSB-first known-vector check passed (value {expected_value})"
        )
    if arming is not None:
        lines.append(
            f"zero-throttle arming: {arming.duration_ms:.3f} ms verified "
            f"across {arming.zero_frames} frame(s); max frame-start gap "
            f"{arming.max_frame_start_gap_us:.6f} us"
        )
    return "\n".join(lines)


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Validate a two-column edge-transition CSV as DShot600",
    )
    parser.add_argument("capture", type=Path, help="CSV with timestamp,level columns")
    parser.add_argument(
        "--time-unit",
        choices=tuple(TIME_UNIT_TO_US),
        default="us",
        help="timestamp unit in the CSV (default: us)",
    )
    parser.add_argument(
        "--timing-tolerance-pct",
        type=float,
        default=DEFAULT_TIMING_TOLERANCE_PCT,
        help="allowed DShot high-width and bit-period error (default: 20)",
    )
    parser.add_argument(
        "--max-edges",
        type=int,
        default=DEFAULT_MAX_EDGES,
        help=f"resource bound for parsed transitions (default: {DEFAULT_MAX_EDGES})",
    )
    parser.add_argument(
        "--minimum-frame-gap-us",
        type=float,
        default=DEFAULT_MINIMUM_FRAME_GAP_US,
        help="minimum LOW time between frames (default: 2 us)",
    )
    parser.add_argument(
        "--zero-arming-ms",
        type=float,
        help="require zero-throttle frames for this interval from the first frame",
    )
    parser.add_argument(
        "--max-arming-frame-gap-us",
        type=float,
        help="optional maximum frame-start gap during the zero-arming interval",
    )
    parser.add_argument(
        "--expect-value",
        type=int,
        help="require every frame to match this known stop/throttle value",
    )
    parser.add_argument(
        "--expect-telemetry",
        type=int,
        choices=(0, 1),
        help="also require this telemetry-request bit (requires --expect-value)",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_argument_parser()
    args = parser.parse_args(argv)
    if args.max_arming_frame_gap_us is not None and args.zero_arming_ms is None:
        parser.error("--max-arming-frame-gap-us requires --zero-arming-ms")
    if args.expect_telemetry is not None and args.expect_value is None:
        parser.error("--expect-telemetry requires --expect-value")

    try:
        transitions = read_transitions(args.capture, args.time_unit, args.max_edges)
        analysis = analyze_transitions(
            transitions,
            timing_tolerance_pct=args.timing_tolerance_pct,
            minimum_frame_gap_us=args.minimum_frame_gap_us,
        )
        if args.expect_value is not None:
            verify_expected_stream(
                analysis.frames,
                args.expect_value,
                bool(args.expect_telemetry) if args.expect_telemetry is not None else None,
            )
        arming = None
        if args.zero_arming_ms is not None:
            arming = verify_zero_throttle_arming(
                analysis.frames,
                args.zero_arming_ms,
                args.max_arming_frame_gap_us,
            )
    except CaptureError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    print(_format_report(analysis, arming, args.expect_value))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
