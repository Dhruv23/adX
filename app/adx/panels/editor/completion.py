"""Completion (phase_7.md 4.6): section names, the keys a section takes, and full
parameter paths - which are otherwise impossible to remember.

The parameter names come from the engine's type catalog (every instrument's and
effect's descriptor table, the same ParamRegistry the parser checks ``PARAM`` lines
against), and the entity names from the open project - so ``channel.Lead.`` completes
to the parameters Lead's instrument actually has.
"""

from __future__ import annotations

from typing import Any

from adx import engine_bridge
from adx.engine_bridge import Project

#: Section types, in canonical order (docs/adx-format-v2.md 4.1).
SECTIONS = (
    "PROJECT",
    "TEMPO",
    "METER",
    "CHANNEL",
    "PATTERN",
    "PLAYLIST",
    "MIXER",
    "MARKERS",
    "CONTROLLER",
    "ARRANGEMENT",
)

#: What a line may begin with, by section (docs/adx-format-v2.md 7).
KEYS: dict[str, tuple[str, ...]] = {
    "PROJECT": ("ADX_VERSION=", "TITLE=", "AUTHOR=", "CREATED=", "TUNING=", "LOOP="),
    "CHANNEL": (
        "INSTRUMENT=",
        "VOICEBANK=",
        "OUTPUT=",
        "POLYPHONY=",
        "STEAL=",
        "VOLUME=",
        "PAN=",
        "PITCH=",
        "MUTE=",
        "SOLO=",
        "COLOR=",
        "PARAM ",
        "ARP ",
        "ZONE ",
    ),
    "PATTERN": ("LENGTH=", "COLOR=", "NOTES ", "MINI ", "AUTOMATION "),
    "PLAYLIST": ("TRACK ", "PATTERN ", "AUDIO "),
    "ARRANGEMENT": ("TRACK ", "PATTERN ", "AUDIO "),
    "MIXER": ("INSERT ", "SLOT ", "SEND ", "ROUTE "),
    "CONTROLLER": ("TYPE=", "TARGET=", "SOURCE=", "SHAPE=", "PERIOD="),
}

#: Mini-notation's words (docs/mininotation.md).
MINI_WORDS = (
    "bd",
    "sn",
    "sd",
    "cp",
    "rim",
    "hh",
    "oh",
    "cy",
    "cr",
    "rd",
    "lt",
    "mt",
    "ht",
    "perc",
    "rev",
    "fast",
    "every",
    "scale=",
)

#: Strip parameters every channel and insert has (engine/project/ParamRegistry.cpp).
CHANNEL_STRIP = ("volume", "pan", "pitch")
INSERT_STRIP = ("gain", "pan", "width")


class Completions:
    """Candidate words for a cursor, from the engine's tables and the open project."""

    def __init__(self) -> None:
        self._catalog: dict[str, list[str]] | None = None

    def catalog(self) -> dict[str, list[str]]:
        """Parameter names by instrument or effect type (one engine call, kept)."""
        if self._catalog is None:
            self._catalog = {
                str(entry["name"]): [str(p["name"]) for p in entry["params"]]
                for entry in engine_bridge.type_catalog()
            }
        return self._catalog

    def types(self, kind: str) -> list[str]:
        """Instrument or effect type names."""
        return sorted(
            str(entry["name"]) for entry in engine_bridge.type_catalog() if entry["kind"] == kind
        )

    def param_paths(self, project: Project | None) -> list[str]:
        """Every parameter path in the project: ``channel.Lead.filter.cutoff``,
        ``insert.2.gain``, ``insert.2.slot.1.mix``..."""
        if project is None:
            return []
        catalog = self.catalog()
        paths: list[str] = []
        for channel in project.channels_info():
            base = f"channel.{channel['name']}."
            paths += [base + name for name in CHANNEL_STRIP]
            paths += [base + name for name in catalog.get(str(channel["instrument"]), [])]
        mixer: dict[str, Any] = project.mixer_info()
        for insert in mixer["inserts"]:
            base = f"insert.{insert['id']}."
            paths += [base + name for name in INSERT_STRIP]
            for slot in insert["slots"]:
                slot_base = f"{base}slot.{slot['id']}."
                paths += [slot_base + "mix"]
                paths += [slot_base + name for name in catalog.get(str(slot.get("type", "")), [])]
        return paths

    def candidates(
        self, section: str, line: str, word: str, project: Project | None, in_mini: bool
    ) -> list[str]:
        """Words that complete ``word`` at the end of ``line`` in ``section``."""
        stripped = line.lstrip()
        options: list[str] = []
        if in_mini:
            options = list(MINI_WORDS)
        elif stripped.startswith("[") or not stripped or stripped == word:
            options = [f"[{name}" for name in SECTIONS] if stripped.startswith("[") else []
            options += list(KEYS.get(section, ()))
        if not in_mini:
            if stripped.startswith("INSTRUMENT="):
                options += self.types("instrument")
            elif stripped.startswith("SLOT ") or stripped.startswith("TYPE="):
                options += self.types("effect")
            elif stripped.startswith("PARAM "):
                options += sorted({p for params in self.catalog().values() for p in params})
            if "." in word or stripped.startswith(("AUTOMATION", "TARGET=")):
                options += self.param_paths(project)
            if project is not None and stripped.startswith(("NOTES", "MINI", "PATTERN ")):
                options += (
                    project.channels()
                    if not stripped.startswith("PATTERN ")
                    else project.patterns()
                )
        lowered = word.lower()
        seen: set[str] = set()
        out = []
        for option in options:
            if option.lower().startswith(lowered) and option != word and option not in seen:
                seen.add(option)
                out.append(option)
        return out
