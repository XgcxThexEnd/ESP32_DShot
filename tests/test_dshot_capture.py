import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from analyze_dshot_capture import (  # noqa: E402
    CaptureError,
    DSHOT600_BIT_PERIOD_US,
    Transition,
    analyze_transitions,
    dshot_checksum,
    read_transitions,
    verify_expected_stream,
    verify_zero_throttle_arming,
)


def make_word(value: int, telemetry: bool = False) -> int:
    payload = (value << 1) | int(telemetry)
    return (payload << 4) | dshot_checksum(payload)


def reverse_16(word: int) -> int:
    return int(f"{word:016b}"[::-1], 2)


def write_capture(tmp_path: Path, words, *, gap_us: float = 50.0, bad_width=None) -> Path:
    path = tmp_path / "capture.csv"
    timestamp = 0.0
    rows = ["time_us,level"]
    for word_index, word in enumerate(words):
        for bit_index in range(15, -1, -1):
            bit = (word >> bit_index) & 1
            width = 1.25 if bit else 0.625
            if bad_width == (word_index, 15 - bit_index):
                width = 0.25
            rows.append(f"{timestamp:.9f},1")
            rows.append(f"{timestamp + width:.9f},0")
            timestamp += DSHOT600_BIT_PERIOD_US
        timestamp += gap_us
    path.write_text("\n".join(rows) + "\n", encoding="utf-8")
    return path


def load_analysis(path: Path):
    return analyze_transitions(read_transitions(path))


def test_decodes_msb_first_frames_and_telemetry(tmp_path: Path) -> None:
    path = write_capture(tmp_path, [make_word(0), make_word(48, True), make_word(2047)])

    analysis = load_analysis(path)

    assert [frame.value for frame in analysis.frames] == [0, 48, 2047]
    assert [frame.telemetry for frame in analysis.frames] == [False, True, False]
    assert all(len(frame.bits) == 16 for frame in analysis.frames)
    assert min(analysis.inter_frame_low_us) >= 50.0


def test_rejects_bad_pulse_timing(tmp_path: Path) -> None:
    path = write_capture(tmp_path, [make_word(100)], bad_width=(0, 3))

    with pytest.raises(CaptureError, match="HIGH width"):
        load_analysis(path)


def test_rejects_checksum_error(tmp_path: Path) -> None:
    path = write_capture(tmp_path, [make_word(100) ^ 0x01])

    with pytest.raises(CaptureError, match="checksum mismatch"):
        load_analysis(path)


def test_known_vector_rejects_lsb_first_wire_order(tmp_path: Path) -> None:
    path = write_capture(tmp_path, [reverse_16(make_word(48))])

    analysis = load_analysis(path)
    assert analysis.frames[0].value != 48
    with pytest.raises(CaptureError, match="wrong wire order or command"):
        verify_expected_stream(analysis.frames, expected_value=48)


def test_known_vector_accepts_msb_first_wire_order(tmp_path: Path) -> None:
    path = write_capture(tmp_path, [make_word(100, True)] * 2)

    verify_expected_stream(
        load_analysis(path).frames,
        expected_value=100,
        expected_telemetry=True,
    )


def test_known_vector_rejects_bit_order_ambiguous_stop(tmp_path: Path) -> None:
    path = write_capture(tmp_path, [make_word(0)])

    with pytest.raises(CaptureError, match="bit-order ambiguous"):
        verify_expected_stream(load_analysis(path).frames, expected_value=0)


@pytest.mark.parametrize("reserved", [1, 17, 47])
def test_rejects_reserved_command_values(tmp_path: Path, reserved: int) -> None:
    path = write_capture(tmp_path, [make_word(reserved)])

    with pytest.raises(CaptureError, match="reserved DShot command"):
        load_analysis(path)


def test_rejects_partial_frames_and_non_transition_edges(tmp_path: Path) -> None:
    path = write_capture(tmp_path, [make_word(100)])
    rows = path.read_text(encoding="utf-8").splitlines()
    path.write_text("\n".join(rows[:-2]) + "\n", encoding="utf-8")
    with pytest.raises(CaptureError, match="expected 16 pulses"):
        load_analysis(path)

    path.write_text("time_us,level\n0,0\n1,0\n", encoding="utf-8")
    with pytest.raises(CaptureError, match="no preceding rising edge"):
        load_analysis(path)


def test_zero_throttle_arming_interval_passes(tmp_path: Path) -> None:
    path = write_capture(tmp_path, [make_word(0)] * 16 + [make_word(48)])
    analysis = load_analysis(path)

    result = verify_zero_throttle_arming(
        analysis.frames,
        duration_ms=1.0,
        max_frame_start_gap_us=100.0,
    )

    assert result.zero_frames >= 14
    assert result.max_frame_start_gap_us < 100.0


def test_zero_throttle_arming_rejects_nonzero_and_short_capture(tmp_path: Path) -> None:
    path = write_capture(tmp_path, [make_word(0), make_word(48)] + [make_word(0)] * 14)
    with pytest.raises(CaptureError, match="nonzero value 48"):
        verify_zero_throttle_arming(load_analysis(path).frames, duration_ms=1.0)

    path = write_capture(tmp_path, [make_word(0)] * 4)
    with pytest.raises(CaptureError, match="capture covers only"):
        verify_zero_throttle_arming(load_analysis(path).frames, duration_ms=1.0)


def test_zero_throttle_arming_can_enforce_frame_cadence(tmp_path: Path) -> None:
    path = write_capture(tmp_path, [make_word(0)] * 8, gap_us=200.0)

    with pytest.raises(CaptureError, match="frame-start gap"):
        verify_zero_throttle_arming(
            load_analysis(path).frames,
            duration_ms=1.0,
            max_frame_start_gap_us=100.0,
        )


def test_timestamp_units_and_cli_entrypoint(tmp_path: Path, capsys) -> None:
    path = write_capture(tmp_path, [make_word(48), make_word(48)])
    microsecond_rows = path.read_text(encoding="utf-8").splitlines()[1:]
    second_rows = ["time_s,level"]
    for row in microsecond_rows:
        timestamp, level = row.split(",")
        second_rows.append(f"{float(timestamp) / 1_000_000.0:.12f},{level}")
    path.write_text("\n".join(second_rows) + "\n", encoding="utf-8")

    from analyze_dshot_capture import main

    assert main([str(path), "--time-unit", "s", "--expect-value", "48"]) == 0
    assert "PASS: 2 DShot600 frame(s)" in capsys.readouterr().out


@pytest.mark.parametrize(
    ("first_row", "message"),
    [
        ("not-a-time,1", "invalid timestamp"),
        ("0,not-a-level", "invalid logic level"),
    ],
)
def test_malformed_first_data_row_is_not_silently_used_as_header(
    tmp_path: Path,
    first_row: str,
    message: str,
) -> None:
    path = tmp_path / "capture.csv"
    path.write_text(f"{first_row}\n1,1\n2,0\n", encoding="utf-8")

    with pytest.raises(CaptureError, match=message):
        read_transitions(path)


@pytest.mark.parametrize(
    "transitions",
    [
        [Transition(0.0, 1), Transition(float("nan"), 0)],
        [Transition(1.0, 1), Transition(1.0, 0)],
        [Transition(0.0, 2), Transition(1.0, 0)],
    ],
)
def test_direct_analysis_rejects_invalid_transition_objects(transitions) -> None:
    with pytest.raises(CaptureError):
        analyze_transitions(transitions)
