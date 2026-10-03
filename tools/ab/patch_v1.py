"""Write fixed/AudioEngine.cpp/.h: the archived engine minus two scheduling bugs.

The Tranche A reference (phase_4.md §4.3) is meant to pin the archived engine's
*sound* - its per-voice DSP and its effects - which is what Phase 4 ports. Its
scheduling half is what Phase 3 replaced, and two of its defects change the
reference audibly:

  * note-off by pitch alone (handleNoteOff): a note-off on one track releases every
    voice at that pitch on every track. In suffocation.adx the Lead's note-offs cut
    the Vocal's notes short - 3 dB of the Vocal's level.
  * one shared 64-voice pool: muted or not, every track's notes compete for the same
    voices, so a dense passage steals across tracks.

This script copies the archive's AudioEngine.cpp/.h into fixed/, under the same names,
with exactly those two things changed - note-offs carry their track, and the pool holds
512 voices. build_v1.cmd builds v1render.exe (the archive as it was) and v1fixed.exe
(fixed/ first on the include path, so every file sees the patched class). Nothing under
_archive/ is modified.
"""

from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parents[1] / "_archive" / "src-cpp"


def replace_once(text: str, old: str, new: str, what: str) -> str:
    if text.count(old) != 1:
        raise SystemExit(f"patch_v1: expected exactly one '{what}' in the archive source")
    return text.replace(old, new)


def main() -> int:
    source = (SRC / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")
    header = (SRC / "include" / "AudioEngine.h").read_text(encoding="utf-8")

    header = replace_once(
        header, "std::array<Voice, 64> m_voices;", "std::array<Voice, 512> m_voices;", "pool"
    )
    header = replace_once(
        header,
        "std::array<Voice, 512> m_voices;",
        "std::array<Voice, 512> m_voices;\n    int m_noteOffTrack = -1;",
        "pool member",
    )
    source = replace_once(
        source,
        "handleNoteOff(offEvt);",
        "m_noteOffTrack = ev.trackIndex;\n                    handleNoteOff(offEvt);\n"
        "                    m_noteOffTrack = -1;",
        "scheduled note-off",
    )
    source = replace_once(
        source,
        "if (voice.active && voice.pitch == event.pitch && voice.envState != EnvState::Release)",
        "if (voice.active && voice.pitch == event.pitch &&\n"
        "            (m_noteOffTrack < 0 || voice.trackIndex == m_noteOffTrack) &&\n"
        "            voice.envState != EnvState::Release)",
        "note-off match",
    )
    fixed = HERE / "fixed"
    fixed.mkdir(exist_ok=True)
    (fixed / "AudioEngine.h").write_text(header, encoding="utf-8")
    (fixed / "AudioEngine.cpp").write_text(source, encoding="utf-8")
    print("wrote tools/ab/fixed/AudioEngine.cpp/.h")
    return 0


if __name__ == "__main__":
    sys.exit(main())
