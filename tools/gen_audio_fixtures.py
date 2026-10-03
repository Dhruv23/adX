"""Generate the compressed audio fixtures for the decode tests.

    python tools/gen_audio_fixtures.py

Writes tests/data/audio/tone.{flac,ogg,mp3}: 0.25 s at 44.1 kHz, stereo, a 440 Hz
sine on the left and 660 Hz on the right, each at amplitude 0.5 (ffmpeg's sine source
is 1/8 full scale, hence volume=4) - the signal
`decode_all_formats` checks every decoder against. The WAV and AIFF variants are
written by the test itself, so only formats that need an encoder are committed. Needs
ffmpeg on PATH; the outputs are committed, so the build never runs this.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
OUT = REPO / "tests" / "data" / "audio"
SOURCE = (
    "sine=frequency=440:sample_rate=44100:duration=0.25,volume=4[l];"
    "sine=frequency=660:sample_rate=44100:duration=0.25,volume=4[r];"
    "[l][r]join=inputs=2:channel_layout=stereo"
)
FORMATS = {
    "tone.flac": ["-c:a", "flac"],
    "tone.ogg": ["-c:a", "libvorbis", "-q:a", "6"],
    "tone.mp3": ["-c:a", "libmp3lame", "-b:a", "192k"],
}


def main() -> int:
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        raise SystemExit("ffmpeg is not on PATH")
    OUT.mkdir(parents=True, exist_ok=True)
    for name, codec in FORMATS.items():
        target = OUT / name
        # -bitexact and no metadata: the same ffmpeg writes the same bytes twice.
        command = [ffmpeg, "-y", "-loglevel", "error", "-filter_complex", SOURCE]
        command += [*codec, "-bitexact", "-map_metadata", "-1", str(target)]
        subprocess.run(command, check=True)
        print(f"wrote {target.relative_to(REPO)} ({target.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
