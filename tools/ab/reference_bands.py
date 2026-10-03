"""Write the Tranche A gate's reference: suffocation.adx's band levels from the archive.

    python tools/ab/reference_bands.py

Renders docs/examples/suffocation.adx through v1fixed.exe (build_v1.cmd) and writes
its 31 third-octave band levels to tests/data/reference/suffocation_v1_bands.txt, which
`suffocation_spectral_match` compares the engine against. The levels are committed
rather than the 30 MB render they come from; this script and build_v1.cmd regenerate
them from _archive/ alone.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

import engine
from bands import CENTRES, file_levels

SOURCE = engine.REPO_ROOT / "docs" / "examples" / "suffocation.adx"
TARGET = engine.REPO_ROOT / "tests" / "data" / "reference" / "suffocation_v1_bands.txt"
TAIL_SECONDS = "4"

HEADER = """\
# suffocation.adx: 31 third-octave band levels in dB, rendered by the archived engine.
#
# The reference for suffocation_spectral_match (phase_4.md §4.3, the Tranche A gate).
# Rendered by tools/ab/v1fixed.exe - _archive/src-cpp's engine with two scheduling
# bugs removed, as tools/ab/patch_v1.py describes - at its native 44.1 kHz with a
# {tail} s tail, and measured as tools/ab/bands.py measures: the mono sum, a
# 65536-point Hann-windowed Welch average with 50 % overlap, power summed per band.
# Regenerate with: python tools/ab/reference_bands.py
#
# centre_hz level_db
"""


def main() -> int:
    if not engine.V1_FIXED.exists():
        raise SystemExit(f"{engine.V1_FIXED} missing: run tools/ab/build_v1.cmd")
    with tempfile.TemporaryDirectory() as scratch:
        wav = Path(scratch) / "suffocation_v1.wav"
        subprocess.run(
            [str(engine.V1_FIXED), str(SOURCE), str(wav), TAIL_SECONDS],
            check=True,
            capture_output=True,
        )
        levels = file_levels(wav)
    TARGET.parent.mkdir(parents=True, exist_ok=True)
    rows = "".join(f"{c:.3f} {v:.4f}\n" for c, v in zip(CENTRES, levels, strict=True))
    TARGET.write_text(HEADER.format(tail=TAIL_SECONDS) + rows, encoding="utf-8", newline="\n")
    print(f"wrote {TARGET.relative_to(engine.REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
