"""Collect-and-save from Python (phase_8.md 3.7): the samples are copied, the paths
rewritten, nothing else in the file changes, and the bundle opens with the originals
gone."""

from __future__ import annotations

import pathlib
import shutil
import struct
import zipfile

from adx.engine_bridge import bundle_project
from adx.format import validate_text


def _write_wav(path: pathlib.Path, frames: int = 4800) -> None:
    """A short 16-bit stereo WAV of a quiet ramp."""
    data = b"".join(struct.pack("<hh", i % 2000, -(i % 2000)) for i in range(frames))
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        36 + len(data),
        b"WAVE",
        b"fmt ",
        16,
        1,
        2,
        48000,
        48000 * 4,
        4,
        16,
        b"data",
        len(data),
    )
    path.write_bytes(header + data)


def _project_text(sample: pathlib.Path) -> str:
    # Comments, odd spacing and an unknown key: everything a bundle must leave alone.
    return (
        "[PROJECT]\n"
        "ADX_VERSION=2\n"
        'TITLE="Bundle me"   ; a trailing comment\n'
        "\n"
        "[TEMPO]\n"
        "0:0:0 120\n"
        "\n"
        "[PLAYLIST]\n"
        'TRACK 1 name="Loop"\n'
        f'  AUDIO "{sample.as_posix()}" 0:0:0\n'
        f'  AUDIO "{sample.as_posix()}" 1:0:0\n'
        "\n"
        "[MIXER]\n"
        'INSERT 1 name="Master"\n'
    )


def test_bundle_collects_rewrites_and_survives_the_originals(tmp_path: pathlib.Path) -> None:
    """bundle_collects_and_rewrites and bundle_preserves_formatting, from Python."""
    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    sample = elsewhere / "loop.wav"
    _write_wav(sample)
    source = tmp_path / "work" / "song.adx"
    source.parent.mkdir()
    text = _project_text(sample)
    source.write_text(text, encoding="utf-8", newline="")

    destination = tmp_path / "bundle"
    result = bundle_project(str(source), str(destination), zip_it=True)
    assert result["ok"], result["error"]
    assert result["copied"] == 1
    assert result["missing"] == []
    assert (destination / "samples" / "loop.wav").read_bytes() == sample.read_bytes()

    bundled = pathlib.Path(result["project"]).read_text(encoding="utf-8")
    # Every line but the two AUDIO lines is byte-identical.
    before, after = text.splitlines(), bundled.splitlines()
    assert len(before) == len(after)
    changed = [i for i, (a, b) in enumerate(zip(before, after, strict=True)) if a != b]
    assert [before[i].split()[0] for i in changed] == ["AUDIO", "AUDIO"]
    assert all("samples/loop.wav" in after[i] for i in changed)

    # The originals gone, the bundle still finds its sample.
    shutil.rmtree(elsewhere)
    diagnostics = validate_text(bundled, str(destination), check_sample_files=True)
    assert not [item for item in diagnostics if item.is_error]

    with zipfile.ZipFile(result["zip"]) as archive:
        names = archive.namelist()
        assert archive.testzip() is None
    assert "bundle/song.adx" in names
    assert "bundle/samples/loop.wav" in names


def test_bundle_reports_what_it_cannot_find(tmp_path: pathlib.Path) -> None:
    """A missing sample is left as written and reported, and the rest still bundles."""
    source = tmp_path / "song.adx"
    source.write_text(_project_text(tmp_path / "gone.wav"), encoding="utf-8")
    result = bundle_project(str(source), str(tmp_path / "out"))
    assert result["ok"], result["error"]
    assert result["copied"] == 0
    assert len(result["missing"]) == 1
