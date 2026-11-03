"""Each effect type's cost on the benchmark (phase_10.md 4.5): every one of the 144 slots
set to one type, the instruments silent, rendered offline; the load over the silent
baseline is that type's cost at 144 instances."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bench"))

import make_project  # noqa: E402
import profile_parts as parts  # noqa: E402

from adx.engine_bridge import type_catalog  # noqa: E402


def main() -> int:
    folder = ROOT / "build" / "bench" / "effects"
    folder.mkdir(parents=True, exist_ok=True)
    base = make_project.build(folder, bars=parts.BARS).read_text(encoding="utf-8")
    silent = parts.only_type(parts.without_slots(base, keep_plugins=False), "none")
    baseline = parts.timed(silent, folder, "baseline")
    print(f"{'baseline (no effects, silent)':32} {baseline:6.1f} %")
    for kind in sorted(t["name"] for t in type_catalog() if t["kind"] == "effect"):
        text = re.sub(
            r"(  SLOT \d+) (?!clap)\S+",
            lambda m: f"{m.group(1)} {kind}",
            parts.only_type(base, "none"),
        )
        text = "\n".join(l for l in text.splitlines() if " clap plugin=" not in l)
        load = parts.timed(text, folder, kind)
        print(f"{kind:32} {load:6.1f} %   (+{load - baseline:.1f})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
