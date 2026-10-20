"""What the browser lists (phase_6.md 4.6): sample folders, the preset library, the
project's patterns, the effect types, plugins (Phase 9's) and the user's favourites.

Each entry knows the drag payload it carries (dnd.py) and the words a search matches.
The tree is rebuilt from fresh reads; nothing here outlives a refresh.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

from adx import dnd

PROJECT_SUFFIXES = (".adx",)
AUDIO_SUFFIXES = (".wav", ".flac", ".mp3", ".ogg", ".aif", ".aiff")
PRESET_SUFFIX = ".adxpreset"


@dataclass(frozen=True, slots=True)
class Entry:
    """One browser row."""

    label: str
    kind: str  # "folder", "audio", "project", "preset", "pattern", "effect", "note"
    path: str = ""
    payload: dnd.Payload | None = None
    tags: tuple[str, ...] = ()
    children: tuple[Entry, ...] = field(default_factory=tuple)

    def matches(self, words: list[str]) -> bool:
        """Every word is in the label, a tag or the path's name."""
        haystack = " ".join((self.label, *self.tags, Path(self.path).name)).lower()
        return all(word in haystack for word in words)


def folder_entries(root: str, depth: int = 0, limit: int = 400) -> tuple[Entry, ...]:
    """A folder's audio files, projects and presets, and its subfolders (lazily shallow)."""
    base = Path(root)
    if not base.is_dir():
        return ()
    out: list[Entry] = []
    try:
        children = sorted(base.iterdir(), key=lambda p: (not p.is_dir(), p.name.lower()))
    except OSError:
        return ()
    for path in children[:limit]:
        if path.name.startswith("."):
            continue
        suffix = path.suffix.lower()
        if path.is_dir():
            inner = folder_entries(str(path), depth + 1, limit) if depth < 2 else ()
            if depth >= 2 or inner:
                out.append(Entry(path.name, "folder", str(path), children=inner))
        elif suffix in AUDIO_SUFFIXES:
            out.append(Entry(path.name, "audio", str(path), dnd.SampleFile(str(path))))
        elif suffix in PROJECT_SUFFIXES:
            out.append(Entry(path.name, "project", str(path)))
        elif suffix == PRESET_SUFFIX:
            out.append(Entry(path.stem, "preset", str(path), dnd.Preset(str(path), "instrument")))
    return tuple(out)


def tags_of(preset: dict[str, object]) -> tuple[str, ...]:
    """A scanned preset's tags (scan_presets gives a list; anything else is none)."""
    raw = preset.get("tags")
    return tuple(str(t) for t in raw) if isinstance(raw, list | tuple) else ()


def preset_entries(presets: list[dict[str, object]], tag: str = "") -> tuple[Entry, ...]:
    """The preset library, grouped by pack; ``tag`` keeps only presets carrying it."""
    packs: dict[str, list[Entry]] = {}
    for preset in presets:
        tags = tags_of(preset)
        if tag and tag not in tags:
            continue
        kind = str(preset["kind"])
        entry = Entry(
            f"{preset['name']}  ({preset['type']})",
            "preset",
            str(preset["path"]),
            dnd.Preset(str(preset["path"]), kind),
            tags=(*tags, str(preset["type"]), kind),
        )
        packs.setdefault(str(preset["pack"]) or "user", []).append(entry)
    return tuple(
        Entry(pack, "folder", children=tuple(items)) for pack, items in sorted(packs.items())
    )


def all_tags(presets: list[dict[str, object]]) -> list[str]:
    """Every tag in the library, sorted."""
    tags: set[str] = set()
    for preset in presets:
        tags.update(tags_of(preset))
    return sorted(tags)


def pattern_entries(patterns: list[dict[str, object]]) -> tuple[Entry, ...]:
    """The open project's patterns, dragged onto the playlist."""
    return tuple(
        Entry(str(p["name"]), "pattern", payload=dnd.PatternRef(int(p["id"])))  # type: ignore[call-overload]
        for p in patterns
    )


def effect_entries(effects: list[dict[str, object]]) -> tuple[Entry, ...]:
    """Every effect type, dragged onto a mixer slot."""
    return tuple(
        Entry(
            str(e["name"]),
            "effect",
            payload=dnd.EffectType(str(e["name"])),
            tags=(str(e["summary"]),),
        )
        for e in effects
    )


def filtered(entries: tuple[Entry, ...], words: list[str]) -> tuple[Entry, ...]:
    """Entries matching every word, folders kept when anything inside them matches."""
    if not words:
        return entries
    out: list[Entry] = []
    for entry in entries:
        if entry.children:
            inner = filtered(entry.children, words)
            if inner:
                out.append(
                    Entry(entry.label, entry.kind, entry.path, entry.payload, entry.tags, inner)
                )
        elif entry.matches(words):
            out.append(entry)
    return tuple(out)
