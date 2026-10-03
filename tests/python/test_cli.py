"""``adx validate``, ``fmt``, ``diff`` and ``info`` on the fixture corpus.

Exit codes are part of the contract: a pre-commit hook asks ``adx fmt --check`` a
yes/no question and only reads the status.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys

import pytest

from adx.cli import main

EXAMPLES = ("example.adx", "c418_demo.adx", "stakillaz_demo.adx", "suffocation.adx")


@pytest.fixture
def examples(repo_root: pathlib.Path) -> pathlib.Path:
    """The v1 corpus directory."""
    return repo_root / "docs" / "examples"


@pytest.mark.parametrize("name", EXAMPLES)
def test_validate_corpus_passes(examples: pathlib.Path, name: str) -> None:
    """Every example validates with no errors."""
    assert main(["validate", str(examples / name)]) == 0


def test_validate_reports_errors(
    tmp_path: pathlib.Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """A broken file exits 1 and names the line and column."""
    bad = tmp_path / "bad.adx"
    bad.write_text("[PROJECT]\nADX_VERSION=2\nTUNING=abc\n", encoding="utf-8")
    assert main(["validate", str(bad)]) == 1
    out = capsys.readouterr().out
    assert "bad.adx:3:8: error ADX0007" in out


def test_upgrade_then_check(tmp_path: pathlib.Path, examples: pathlib.Path) -> None:
    """A v1 file upgrades once; the result is already canonical."""
    upgraded = tmp_path / "suffocation.adx"
    source = str(examples / "suffocation.adx")
    assert main(["fmt", "--upgrade", source, "-o", str(upgraded)]) == 0
    assert "ADX_VERSION=2" in upgraded.read_text(encoding="utf-8")
    assert main(["fmt", "--check", str(upgraded)]) == 0
    assert main(["validate", str(upgraded)]) == 0


def test_v1_is_never_rewritten_in_place(examples: pathlib.Path, tmp_path: pathlib.Path) -> None:
    """``fmt -i`` and ``--upgrade`` without ``-o`` both refuse a v1 file."""
    copy = tmp_path / "example.adx"
    original = (examples / "example.adx").read_bytes()
    copy.write_bytes(original)
    assert main(["fmt", "-i", str(copy)]) == 1
    assert main(["fmt", "--upgrade", str(copy)]) == 1
    assert copy.read_bytes() == original


def test_fmt_check_detects_noncanonical(tmp_path: pathlib.Path) -> None:
    """``--check`` exits 1 on a file that would change, 0 after ``-i``."""
    path = tmp_path / "x.adx"
    path.write_text("[PROJECT]\nADX_VERSION=2\nTUNING=440.0\n", encoding="utf-8")
    assert main(["fmt", "--check", str(path)]) == 1
    assert main(["fmt", "-i", str(path)]) == 0
    assert main(["fmt", "--check", str(path)]) == 0


def test_diff_is_semantic(
    tmp_path: pathlib.Path, examples: pathlib.Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """Changing a note and a volume shows up as changes to entities, not lines."""
    before = tmp_path / "a.adx"
    assert main(["fmt", "--upgrade", str(examples / "c418_demo.adx"), "-o", str(before)]) == 0
    text = before.read_text(encoding="utf-8")
    after = tmp_path / "b.adx"
    edited = text.replace("A3 0:0:0 1:0:0 102", "A3 0:0:0 1:0:0 90")
    edited = edited.replace("VOLUME=1", "VOLUME=0.5")
    after.write_text(edited, encoding="utf-8")
    capsys.readouterr()
    assert main(["diff", str(before), str(before)]) == 0
    assert main(["diff", str(before), str(after)]) == 1
    out = capsys.readouterr().out
    assert "~ channel Kalimba: volume" in out
    assert "~ pattern Kalimba: Kalimba: +1 notes -1 notes" in out


def test_info(examples: pathlib.Path, capsys: pytest.CaptureFixture[str]) -> None:
    """``info`` prints counts drawn from the model."""
    assert main(["info", str(examples / "suffocation.adx")]) == 0
    out = capsys.readouterr().out
    assert "notes:           349" in out
    assert "format:          v1" in out
    # phase_4.md 7: the real instruments, not test tones.
    assert "  Kick     additive" in out
    assert "testtone" not in out
    assert "unknown type" not in out


def test_render_is_declared_but_unavailable() -> None:
    """``render`` exists and says when it arrives, rather than being absent."""
    result = subprocess.run(
        [sys.executable, "-m", "adx", "render", "x.adx"],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 2
    assert "Phase 8" in result.stderr


def test_fmt_stdout_preserves_crlf(tmp_path: pathlib.Path) -> None:
    """A CRLF file formatted to stdout keeps CRLF, and does not gain a second CR.

    stdout is a text stream on Windows; writing CRLF text through it doubled every
    line ending until ``fmt`` switched to writing bytes.
    """
    path = tmp_path / "crlf.adx"
    path.write_bytes(b"[PROJECT]\r\nADX_VERSION=2\r\nTUNING=440\r\n")
    result = subprocess.run(
        [sys.executable, "-m", "adx", "fmt", str(path)], capture_output=True, check=True
    )
    assert b"\r\r" not in result.stdout
    assert result.stdout.startswith(b"[PROJECT]\r\nADX_VERSION=2\r\n")
