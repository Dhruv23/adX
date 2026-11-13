"""The agent's half of phase 12's human gate (phase_12.md 6): every sound in the
demonstration set loads, renders without `adx glitch` findings, and is listed in the
browser's Sounds section.

    python tools/packs/check_demo_set.py [--out DIR] [--wannacry]

Each .adxpreset and .adxrack in docs/examples/phase12 is auditioned (its category's
phrase, as the browser's preview plays it) and the render scanned for dropouts, repeats
and clicks; each .adx is rendered as it stands. A sound that needs a library that is not
installed is reported as such, not as a pass.

--wannacry adds G8, local only (FINAL_PLAN 9, the [wannacry] corpus): the user's piano
transcription is exported as a .mid, imported onto one 16-part gm channel (File >
Import MIDI on one GM channel) and rendered twice, on the Windows gm.dls and on the
GeneralUser GS bank from the library store. Nothing derived from it leaves --out.

Writes report.json in --out (default build/phase12-demo) and exits 1 on any failure.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "app"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import audition  # noqa: E402

from adx import engine_bridge  # noqa: E402
from adx.panels.browser.model import sound_entries  # noqa: E402
from adx.testing.glitch import find_glitches  # noqa: E402

DEMO = REPO / "docs" / "examples" / "phase12"
TRANSCRIPTION = REPO / "output" / "transcription" / "wannacry_piano.adx"
BLANK = '[PROJECT]\nADX_VERSION=2\n\n[MIXER]\nINSERT 1 name="Master"\n'
SHIPPED_BANK = "lib://generaluser/GeneralUser-GS.sf2"


def listed(paths: set[str]) -> set[str]:
    """Which of `paths` the browser's Sounds section shows."""
    presets = engine_bridge.scan_presets([str(DEMO)])
    racks = engine_bridge.scan_racks([str(DEMO)])
    seen: set[str] = set()
    stack = list(sound_entries(presets, racks))
    while stack:
        entry = stack.pop()
        stack.extend(entry.children)
        if entry.path:
            seen.add(str(Path(entry.path).resolve()))
    return {p for p in paths if p in seen}


def glitches(wav: Path) -> list[str]:
    audio, rate = audition.read_wav_float(wav)
    return [str(f) for f in find_glitches(audio, rate)]


def check_sound(path: Path, out: Path) -> dict[str, Any]:
    result = audition.audition(path, out=out / "audition")
    needs = [d for d in result.get("diagnostics", []) if "ADX3006" in str(d)]
    found = glitches(Path(result["wav"]))
    return {
        "file": path.name,
        "wav": result["wav"],
        "lufs": result["lufs"],
        "true_peak": result.get("true_peak"),
        "needs_library": [str(d) for d in needs],
        "glitches": found,
        "ok": not found and not needs and result["lufs"] > -70.0,
    }


def render_project(project: engine_bridge.Project, wav: Path) -> dict[str, Any]:
    wav.parent.mkdir(parents=True, exist_ok=True)
    stats = engine_bridge.render_to_file(project, str(wav), tail_seconds=2.0)
    audio, rate = audition.read_wav_float(wav)
    levels = audition.measure(audio, rate)
    found = [str(f) for f in find_glitches(audio, rate)]
    return {
        "wav": str(wav),
        "lufs": levels["lufs"],
        "seconds": audio.shape[0] / rate,
        "hash": stats.get("hash", ""),
        "glitches": found,
        "ok": not found and levels["lufs"] > -70.0,
    }


def check_project(path: Path, out: Path) -> dict[str, Any]:
    project, _ = engine_bridge.Project.load(str(path))
    return {"file": path.name, **render_project(project, out / f"{path.stem}.wav")}


def wannacry(out: Path) -> list[dict[str, Any]]:
    """G8: the transcription as a .mid on one 16-part gm channel, on both banks."""
    if not TRANSCRIPTION.is_file():
        return [{"file": "G8 WannaCry", "ok": False, "error": f"missing {TRANSCRIPTION}"}]
    source, _ = engine_bridge.Project.load(str(TRANSCRIPTION))
    midi = out / "wannacry.mid"
    out.mkdir(parents=True, exist_ok=True)
    engine_bridge.export_midi(source, str(midi))
    results = []
    for label, bank in (("system gm.dls", ""), ("GeneralUser GS", SHIPPED_BANK)):
        project, _ = engine_bridge.Project.loads(BLANK)
        imported = engine_bridge.import_midi(project, str(midi), gm_parts=True)
        text = project.dumps()
        if bank:
            text = text.replace("INSTRUMENT=gm\n", f'INSTRUMENT=gm\nBANK="{bank}"\n', 1)
        stem = "wannacry-gm-" + ("system" if not bank else "generaluser")
        saved = out / f"{stem}.adx"
        saved.write_text(text, encoding="utf-8")
        project, _ = engine_bridge.Project.load(str(saved))
        result = render_project(project, out / f"{stem}.wav")
        results.append(
            {
                "file": f"G8 WannaCry on {label}",
                "project": str(saved),
                "tracks": imported["tracks"],
                "channels": project.channels(),
                **result,
            }
        )
    return results


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=REPO / "build" / "phase12-demo")
    parser.add_argument("--wannacry", action="store_true", help="also G8, local only")
    args = parser.parse_args(argv)
    out: Path = args.out
    out.mkdir(parents=True, exist_ok=True)

    sounds = sorted([*DEMO.glob("*.adxpreset"), *DEMO.glob("*.adxrack")])
    shown = listed({str(p.resolve()) for p in sounds})
    results: list[dict[str, Any]] = []
    for path in sounds:
        entry = check_sound(path, out)
        entry["in_browser"] = str(path.resolve()) in shown
        entry["ok"] = entry["ok"] and entry["in_browser"]
        results.append(entry)
    for path in sorted(DEMO.glob("*.adx")):
        results.append(check_project(path, out))
    if args.wannacry:
        results.extend(wannacry(out / "wannacry"))

    (out / "report.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    for r in results:
        state = "ok" if r["ok"] else "FAILED"
        detail = f"{r.get('lufs', float('nan')):.1f} LUFS"
        if r.get("glitches"):
            detail += f", {len(r['glitches'])} glitch(es)"
        if r.get("needs_library"):
            detail += ", needs a library"
        if r.get("in_browser") is False:
            detail += ", not in the browser"
        if r.get("error"):
            detail = r["error"]
        print(f"{state:6} {r['file']}: {detail}")
    failed = [r for r in results if not r["ok"]]
    print(f"{len(results) - len(failed)}/{len(results)} ok; report in {out / 'report.json'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
