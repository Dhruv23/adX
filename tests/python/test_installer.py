"""The installer (phase_10.md 4.8, 6).

``third_party_generated`` runs everywhere. ``installer_clean_install`` installs a built
MSI, so it runs only where ``ADX_INSTALLER_MSI`` names one: the full check's job on a
fresh hosted runner, which is the clean machine (phase_10.md 7).
"""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def test_third_party_generated() -> None:
    """THIRD-PARTY.md is what the manifest says, and every dependency names a licence."""
    run = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "gen_third_party.py"), "--check"],
        capture_output=True,
        text=True,
        check=False,
    )
    assert run.returncode == 0, run.stderr
    text = (ROOT / "installer" / "THIRD-PARTY.md").read_text(encoding="utf-8")
    assert "Rubber Band Library | v4.0.0 | GPL-2.0-or-later" in text


def test_a_dependency_without_a_licence_fails(tmp_path: pathlib.Path) -> None:
    sys.path.insert(0, str(ROOT / "tools"))
    import gen_third_party

    manifest = tmp_path / "AdxDependencies.cmake"
    manifest.write_text(
        "# adx-license: a | A | MIT | x\nFetchContent_Declare(a\n  GIT_TAG 1.0\n)\n"
        "FetchContent_Declare(b\n  GIT_TAG 2.0\n)\n",
        encoding="utf-8",
    )
    original = gen_third_party.MANIFEST
    gen_third_party.MANIFEST = manifest
    try:
        with pytest.raises(SystemExit, match="above b"):
            gen_third_party.manifest()
    finally:
        gen_third_party.MANIFEST = original


INSTALL_DIR = pathlib.Path(os.environ.get("LOCALAPPDATA", "")) / "Programs" / "adX"


@pytest.mark.skipif(
    not os.environ.get("ADX_INSTALLER_MSI"), reason="needs a built MSI (the full check's job)"
)
def test_installer_clean_install(tmp_path: pathlib.Path) -> None:
    """Install -> launch -> open an example -> render -> uninstall leaves only user data."""
    msi = pathlib.Path(os.environ["ADX_INSTALLER_MSI"])
    user_data = pathlib.Path(tempfile.mkdtemp()) / "projects"
    user_data.mkdir()
    (user_data / "mine.adx").write_text("[PROJECT]\nADX_VERSION=2\n", encoding="utf-8")
    run = subprocess.run(
        ["msiexec", "/i", str(msi), "/qn", "/l*v", str(tmp_path / "install.log")], check=False
    )
    assert run.returncode == 0, (tmp_path / "install.log").read_text(
        encoding="utf-16", errors="replace"
    )[-3000:]
    try:
        python = INSTALL_DIR / "python" / "python.exe"
        assert python.exists()
        # Launch: the app comes up, smoke-reports and quits.
        report = tmp_path / "smoke.json"
        env = dict(
            os.environ,
            ADX_SMOKE=str(report),
            ADX_SETTINGS_DIR=str(tmp_path / "settings"),
            ADX_NO_QUICKSTART="1",
        )
        subprocess.Popen(
            [
                str(INSTALL_DIR / "python" / "pythonw.exe"),
                "-m",
                "adx",
                "gui",
                "--null-audio",
                str(INSTALL_DIR / "examples" / "c418_demo.adx"),
            ],
            env=env,
        )
        deadline = time.monotonic() + 120
        while not report.exists() and time.monotonic() < deadline:
            time.sleep(0.5)
        time.sleep(0.5)
        facts = json.loads(report.read_text(encoding="utf-8"))
        assert facts["visible"]
        assert len(facts["panels"]) >= 11
        # Render an example headless with the installed interpreter.
        out = tmp_path / "c418.wav"
        upgraded = tmp_path / "c418.adx"
        for args in (
            [
                "fmt",
                "--upgrade",
                str(INSTALL_DIR / "examples" / "c418_demo.adx"),
                "-o",
                str(upgraded),
            ],
            ["render", str(upgraded), "-o", str(out)],
        ):
            done = subprocess.run(
                [str(python), "-m", "adx", *args], capture_output=True, text=True, check=False
            )
            assert done.returncode == 0, done.stderr
        assert out.stat().st_size > 100_000
    finally:
        run = subprocess.run(["msiexec", "/x", str(msi), "/qn"], check=False)
    assert run.returncode == 0
    assert not INSTALL_DIR.exists() or not any(INSTALL_DIR.rglob("*")), list(
        INSTALL_DIR.rglob("*")
    )[:10]
    assert (user_data / "mine.adx").exists()
