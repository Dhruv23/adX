"""The export matrix from Python, and ``adx render`` (phase_8.md 3.5, 3.6, 5).

The engine's own tests own the audio (golden_all_formats, stems_sum_to_master and the
rest in tests/cpp/render); these check the Python half: the request a setting turns
into, that every format comes out as a file of its kind, the job's progress and
cancel, and that the CLI is the dialog's export with no Qt in the process.
"""

from __future__ import annotations

import pathlib
import struct
import subprocess
import sys

import pytest

from adx.cli import main
from adx.engine_bridge import ExportJob, Project, export_project
from adx.export_settings import (
    FORMATS,
    ExportSettings,
    format_for,
    parse_normalize,
    parse_tail,
)


@pytest.fixture
def chord(repo_root: pathlib.Path) -> pathlib.Path:
    """A short golden fixture: four bars of chords on one instrument."""
    return repo_root / "tests" / "golden" / "chord_steal.adx"


def _wav_header(raw: bytes) -> tuple[int, int, int, int]:
    """(format tag, channels, rate, bits) of a RIFF or RF64 file."""
    assert raw[:4] in (b"RIFF", b"RF64")
    assert raw[8:12] == b"WAVE"
    at = 12
    while at + 8 <= len(raw):
        tag, size = raw[at : at + 4], struct.unpack("<I", raw[at + 4 : at + 8])[0]
        if tag == b"fmt ":
            fmt, channels, rate = struct.unpack("<HHI", raw[at + 8 : at + 16])
            bits = struct.unpack("<H", raw[at + 22 : at + 24])[0]
            return fmt, channels, rate, bits
        at += 8 + size + (size & 1)
    raise AssertionError("no fmt chunk")


@pytest.mark.parametrize("name", FORMATS)
def test_every_format_writes_a_file_of_its_kind(
    chord: pathlib.Path, tmp_path: pathlib.Path, name: str
) -> None:
    """Each format's file starts as that format, and the engine says how long it is."""
    project, _ = Project.load(str(chord))
    out = tmp_path / f"out.{name}"
    result = export_project(project, ExportSettings(output=str(out), format=name).request())
    assert result["ok"], result["error"]
    (only,) = result["files"]
    assert only["sample_rate"] == 48000
    assert only["frames"] > 48000
    raw = pathlib.Path(only["path"]).read_bytes()
    if name.startswith("wav"):
        fmt, channels, rate, bits = _wav_header(raw)
        expected = {"wav16": (1, 16), "wav24": (1, 24), "wav32f": (3, 32)}[name]
        assert (fmt, bits) == expected or (fmt == 0xFFFE and bits == expected[1])
        assert (channels, rate) == (2, 48000)
    elif name == "flac":
        assert raw[:4] == b"fLaC"
    elif name == "ogg":
        assert raw[:4] == b"OggS"
        assert b"vorbis" in raw[:64]
    else:
        # An MP3 frame header: eleven sync bits, MPEG-1 Layer III.
        assert raw[0] == 0xFF
        assert raw[1] & 0xFE == 0xFA


def test_stems_and_midi_come_out_beside_the_master(
    repo_root: pathlib.Path, tmp_path: pathlib.Path
) -> None:
    """Stems per channel are named after their channel; the MIDI side file is there."""
    project, _ = Project.load(str(repo_root / "tests" / "golden" / "routing_sends.adx"))
    out = tmp_path / "mix.wav"
    settings = ExportSettings(output=str(out), format="wav32f", stems="channels", midi=True)
    result = export_project(project, settings.request())
    assert result["ok"], result["error"]
    labels = [item["label"] for item in result["files"]]
    assert labels == ["master", *project.channels(), "midi"]
    for item in result["files"]:
        assert pathlib.Path(item["path"]).is_file()
    assert (tmp_path / "mix.mid").is_file()


def test_job_reports_progress_and_finishes(chord: pathlib.Path, tmp_path: pathlib.Path) -> None:
    """The threaded job ends done, at progress 1, with what export_project returns."""
    project, _ = Project.load(str(chord))
    request = ExportSettings(output=str(tmp_path / "job.wav")).request()
    job = ExportJob(project, request)
    result = job.result()
    assert job.done
    assert job.progress == pytest.approx(1.0)
    assert result["ok"]
    direct = export_project(project, {**request, "output": str(tmp_path / "direct.wav")})
    assert result["files"][0]["hash"] == direct["files"][0]["hash"]


def test_job_cancels(repo_root: pathlib.Path, tmp_path: pathlib.Path) -> None:
    """Cancelled straight away, a long export reports cancelled rather than ok."""
    project, _ = Project.load(str(repo_root / "docs" / "examples" / "suffocation.adx"))
    job = ExportJob(
        project, ExportSettings(output=str(tmp_path / "long.flac"), format="flac").request()
    )
    job.cancel()
    result = job.result()
    assert result["cancelled"]
    assert not result["ok"]


@pytest.mark.parametrize(
    ("text", "expected"),
    [("4s", (4.0, False)), ("4", (4.0, False)), ("250ms", (0.25, False)), ("silence", (0.0, True))],
)
def test_tail_spellings(text: str, expected: tuple[float, bool]) -> None:
    """The CLI's --tail spellings."""
    assert parse_tail(text) == expected


@pytest.mark.parametrize(
    ("text", "expected"),
    [("-14LUFS", ("lufs", -14.0)), ("-1dB", ("peak", -1.0)), ("-0.3dBTP", ("peak", -0.3))],
)
def test_normalize_spellings(text: str, expected: tuple[str, float]) -> None:
    """The CLI's --normalize spellings."""
    assert parse_normalize(text) == expected


def test_format_follows_the_extension() -> None:
    """No --format: the output's extension decides, and .wav is 24-bit."""
    assert format_for("a.wav") == "wav24"
    assert format_for("a.MP3") == "mp3"
    assert format_for("a.flac") == "flac"
    assert format_for("a.ogg") == "ogg"


def test_render_cli_matches_the_dialog_export(chord: pathlib.Path, tmp_path: pathlib.Path) -> None:
    """render_cli: ``adx render`` writes the same bits as the dialog's export with the same
    settings - both go through ExportSettings - and the CLI process never loads Qt."""
    cli_out = tmp_path / "cli.wav"
    script = (
        "import sys\n"
        "from adx.cli import main\n"
        f"status = main(['render', {str(chord)!r}, '-o', {str(cli_out)!r}, '--format', 'wav16',"
        " '--tail', '1s', '--normalize', '-1dB', '--block', '512'])\n"
        "assert not [m for m in sys.modules if m.startswith('PySide6')], 'Qt was imported'\n"
        "sys.exit(status)\n"
    )
    run = subprocess.run(
        [sys.executable, "-c", script], capture_output=True, text=True, check=False
    )
    assert run.returncode == 0, run.stderr
    assert str(cli_out) in run.stdout

    project, _ = Project.load(str(chord))
    gui_out = tmp_path / "gui.wav"
    settings = ExportSettings(
        output=str(gui_out),
        format="wav16",
        tail_seconds=1.0,
        normalize="peak",
        normalize_target=-1.0,
        block=512,
    )
    result = export_project(project, settings.request())
    assert result["ok"]
    assert cli_out.read_bytes() == gui_out.read_bytes()


def test_render_cli_reports_bad_settings(chord: pathlib.Path, tmp_path: pathlib.Path) -> None:
    """A tail it cannot read is a failure with a message, not a traceback."""
    status = main(["render", str(chord), "-o", str(tmp_path / "x.wav"), "--tail", "soon"])
    assert status == 1


def test_render_cli_stems(repo_root: pathlib.Path, tmp_path: pathlib.Path) -> None:
    """--stems channels --stems-only writes one file per channel and no master."""
    fixture = repo_root / "tests" / "golden" / "routing_sends.adx"
    out = tmp_path / "mix.flac"
    assert (
        main(["render", str(fixture), "-o", str(out), "--stems", "channels", "--stems-only"]) == 0
    )
    assert not out.exists()
    project, _ = Project.load(str(fixture))
    assert len(list(tmp_path.glob("*.flac"))) == len(project.channels())
