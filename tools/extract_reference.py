"""Pull the WannaCry reference audio out of the tutorial video (phase_10.5.md 4.8).

    py -3.12 tools/extract_reference.py [--video PATH] [--out PATH]

Writes output/reference/wannacry.wav (48 kHz stereo float), which
scenario_wannacry_matches_reference compares the agent's render with. The video and
the WAV are local only: both sit under paths .git/info/exclude ignores, and neither,
nor anything made from them, is ever committed.

ffmpeg comes from winget (Gyan.FFmpeg); this finds it on PATH or in winget's folder.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VIDEO = "Ninajirachi & Porter Robinson - WannaCry (Piano Tutorial).mp4"


def ffmpeg() -> str | None:
    found = shutil.which("ffmpeg")
    if found:
        return found
    packages = Path(os.environ.get("LOCALAPPDATA", "")) / "Microsoft" / "WinGet" / "Packages"
    for candidate in sorted(packages.glob("Gyan.FFmpeg*/*/bin/ffmpeg.exe"), reverse=True):
        return str(candidate)
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    root = Path(os.environ.get("ADX_WANNACRY_DIR", ROOT))
    parser.add_argument("--video", type=Path, default=root / VIDEO)
    parser.add_argument("--out", type=Path, default=root / "output" / "reference" / "wannacry.wav")
    args = parser.parse_args()
    if not args.video.exists():
        print(f"extract_reference: no video at {args.video}", file=sys.stderr)
        return 1
    tool = ffmpeg()
    if tool is None:
        print("extract_reference: no ffmpeg (winget install Gyan.FFmpeg)", file=sys.stderr)
        return 1
    args.out.parent.mkdir(parents=True, exist_ok=True)
    run = subprocess.run(
        [
            tool,
            "-y",
            "-v",
            "error",
            "-i",
            str(args.video),
            "-vn",
            "-ac",
            "2",
            "-ar",
            "48000",
            "-c:a",
            "pcm_f32le",
            str(args.out),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if run.returncode != 0:
        print(f"extract_reference: ffmpeg failed:\n{run.stderr}", file=sys.stderr)
        return 1
    print(f"extract_reference: {args.out} ({args.out.stat().st_size / 1e6:.1f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
