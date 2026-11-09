"""The pack generators (phase_12.md 4.10): deterministic, and their output well formed."""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "packs"))

import gen_irs  # noqa: E402
import gen_wavetables  # noqa: E402
from wavio import read_wav, write_wav  # noqa: E402


@pytest.fixture(scope="module")
def tables(tmp_path_factory: pytest.TempPathFactory) -> dict[str, Path]:
    out = tmp_path_factory.mktemp("wavetables")
    return {p.stem: p for p in gen_wavetables.generate(out)}


@pytest.fixture(scope="module")
def irs(tmp_path_factory: pytest.TempPathFactory) -> dict[str, Path]:
    out = tmp_path_factory.mktemp("irs")
    return {p.stem: p for p in gen_irs.generate(out)}


def test_wav_round_trip(tmp_path: Path) -> None:
    x = np.linspace(-1, 1, 1001)
    write_wav(tmp_path / "x.wav", np.stack([x, -x], axis=1), 48000)
    y, rate = read_wav(tmp_path / "x.wav")
    assert rate == 48000
    assert np.max(np.abs(y[:, 0] - x)) < 2e-7
    assert np.max(np.abs(y[:, 1] + x)) < 2e-7


def test_every_table_is_whole_frames_normalised(tables: dict[str, Path]) -> None:
    assert set(tables) == set(gen_wavetables.TABLES)
    for name, path in tables.items():
        data, _ = read_wav(path)
        assert data.shape[1] == 1, name
        assert len(data) % 2048 == 0, name
        frames = data[:, 0].reshape(-1, 2048)
        peaks = np.max(np.abs(frames), axis=1)
        assert np.all(peaks <= 0.981), name
        assert np.all(peaks > 0.97), name
        assert np.all(np.abs(frames.mean(axis=1)) < 1e-3), name


def test_tables_are_band_limited(tables: dict[str, Path]) -> None:
    # Nothing at Nyquist: every frame was built from harmonics 1..1023.
    for name, path in tables.items():
        frames = read_wav(path)[0][:, 0].reshape(-1, 2048)
        nyquist = np.abs(np.fft.rfft(frames, axis=1)[:, -1]) / 1024
        assert np.max(nyquist) < 1e-4, name


def test_drawbar_registration_is_its_harmonics(tables: dict[str, Path]) -> None:
    frames = read_wav(tables["organ-drawbars"])[0][:, 0].reshape(-1, 2048)
    spectrum = np.abs(np.fft.rfft(frames[0]))  # 888000000: 16', 5 1/3', 8'
    loud = set(np.flatnonzero(spectrum > spectrum.max() * 0.01))
    assert loud == {1, 2, 3}


def rt60(ir: np.ndarray) -> float:
    """Schroeder backward integration, the -5 to -25 dB slope extrapolated to 60 dB."""
    energy = np.cumsum((ir[::-1] ** 2).sum(axis=1))[::-1]
    db = 10 * np.log10(energy / energy[0] + 1e-30)
    start = int(np.argmax(db <= -5))
    stop = int(np.argmax(db <= -25))
    slope = (db[stop] - db[start]) / ((stop - start) / gen_irs.RATE)
    return -60.0 / slope


@pytest.mark.parametrize(
    ("name", "target"),
    [("room-small", 0.45), ("room-medium", 0.9), ("room-large", 1.8), ("plate-long", 3.2)],
)
def test_reverb_decay_matches(irs: dict[str, Path], name: str, target: float) -> None:
    ir, rate = read_wav(irs[name])
    assert rate == 48000
    # Broadband: the faster-decaying top pulls the measured time below the low band's.
    assert target * 0.55 < rt60(ir) < target * 1.1


def test_irs_well_formed(irs: dict[str, Path]) -> None:
    assert set(irs) == set(gen_irs.IRS)
    for name, path in irs.items():
        ir, rate = read_wav(path)
        assert rate == 48000, name
        assert ir.shape[1] == 2, name
        assert np.all(np.isfinite(ir)), name
        assert 0.85 < np.max(np.abs(ir)) <= 0.901, name
        assert np.max(np.abs(ir[-16:])) < 1e-3, name  # faded out, no click at the end


def test_committed_output_matches_generators(tables: dict[str, Path], irs: dict[str, Path]) -> None:
    generated = REPO / "docs" / "examples" / "generated"
    for folder, made in (("wavetables", tables), ("irs", irs)):
        for name, path in made.items():
            committed = generated / folder / f"{name}.wav"
            assert committed.exists(), f"{committed} missing: run tools/packs/gen_{folder}.py"
            a = read_wav(committed)[0]
            b = read_wav(path)[0]
            assert a.shape == b.shape, name
            # Byte-exact on one numpy build; a different FFT may move the last bit.
            assert np.max(np.abs(a - b)) < 1e-5, name
