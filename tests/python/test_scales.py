"""Scale presets, custom scales and the scale lock (phase_11.6.md 4.1, 5)."""

from __future__ import annotations

import pathlib

import pytest
from PySide6.QtCore import QSettings

from adx.engine_bridge import PPQ
from adx.mainwindow import MainWindow
from adx.panels.piano_roll import scales
from adx.panels.piano_roll.custom_scale_dialog import CustomScaleDialog

#: The added presets' pitch classes above the root, entered by hand from the published
#: interval sets, not derived from scales.SCALES.
ADDED = {
    "Harmonic major": {0, 2, 4, 5, 7, 8, 11},
    "Diminished (whole-half)": {0, 2, 3, 5, 6, 8, 9, 11},
    "Diminished (half-whole)": {0, 1, 3, 4, 6, 7, 9, 10},
    "Hungarian minor": {0, 2, 3, 6, 7, 8, 11},
    "Hungarian major": {0, 3, 4, 6, 7, 9, 10},
    "Neapolitan major": {0, 1, 3, 5, 7, 9, 11},
    "Neapolitan minor": {0, 1, 3, 5, 7, 8, 11},
    "Enigmatic": {0, 1, 4, 6, 8, 10, 11},
    "Persian": {0, 1, 4, 5, 6, 8, 11},
    "8-tone Spanish": {0, 1, 3, 4, 5, 6, 8, 10},
    "Gypsy (Romani)": {0, 2, 3, 6, 7, 8, 10},
    "Arabic (double harmonic)": {0, 1, 4, 5, 7, 8, 11},
    "Ahava Rabba (Phrygian dominant)": {0, 1, 4, 5, 7, 8, 10},
    "Hirajoshi": {0, 2, 3, 7, 8},
    "Iwato": {0, 1, 5, 6, 10},
    "Kumoi": {0, 2, 3, 7, 9},
    "Pelog": {0, 1, 3, 7, 8},
}


@pytest.mark.parametrize("name", sorted(ADDED))
def test_added_preset_masks(name: str) -> None:
    mask = scales.mask_of(name)
    assert scales.degrees(0, mask) == sorted(ADDED[name])
    # Snapping stays inside the scale from any root.
    for root in (0, 5, 11):
        assert all(
            scales.in_scale(scales.snap_pitch(p, root, mask), root, mask) for p in range(128)
        )


def test_preset_list() -> None:
    assert len(scales.SCALES) == 31
    masks = [scales.mask_of(name) for name in scales.SCALES]
    assert len(set(masks)) == len(masks), "two presets share a mask"
    assert all(m & 1 for m in masks), "every scale contains its root"


def test_custom_scale_round_trip(tmp_path: pathlib.Path) -> None:
    path = str(tmp_path / "settings.ini")
    store = scales.CustomScales(QSettings(path, QSettings.Format.IniFormat))
    store.save("My dorian", 0b0110_1010_1101)
    store.save("Sparse", 0b1001_0001)
    store.save("My dorian", 0b0010_1010_1101)  # same name: overwritten
    again = scales.CustomScales(QSettings(path, QSettings.Format.IniFormat))
    assert again.names() == ["My dorian", "Sparse"]
    assert again.mask("My dorian") == 0b0010_1010_1101
    again.delete("Sparse")
    assert scales.CustomScales(QSettings(path, QSettings.Format.IniFormat)).names() == ["My dorian"]
    for bad in ("", "  ", "Major", "No scale", scales.CUSTOM_ENTRY, "a/b"):
        assert not scales.CustomScales.valid_name(bad)
        with pytest.raises(ValueError, match="not a usable scale name"):
            store.save(bad, 0b101)
    # The root's bit is always in the mask.
    store.save("Rootless", 0b1000)
    assert store.mask("Rootless") == 0b1001


def test_custom_scale_dialog_saves(qapp: object, tmp_path: pathlib.Path) -> None:
    store = scales.CustomScales(QSettings(str(tmp_path / "s.ini"), QSettings.Format.IniFormat))
    dialog = CustomScaleDialog(store, root=2)
    assert dialog.toggles[0].isChecked()
    assert not dialog.toggles[0].isEnabled()
    assert dialog.toggles[1].text().endswith("D#")  # labelled from the root
    dialog.set_mask(0b1000_1001)
    dialog.name_edit.setText("Major")
    assert not dialog.save_button.isEnabled()
    dialog.name_edit.setText("Three")
    assert dialog.save_button.isEnabled()
    dialog.save()
    assert dialog.outcome == "saved"
    assert store.mask("Three") == 0b1000_1001
    edit = CustomScaleDialog(store, "Three")
    assert edit.scale_mask() == 0b1000_1001
    edit.delete()
    assert store.names() == []


def _scale_index(roll: object, name: str) -> int:
    return roll.scale_box.findText(name)  # type: ignore[attr-defined]


def test_lock_resnaps_existing_notes(studio: MainWindow) -> None:
    roll = studio.piano_roll
    roll.select_clip("P1", "Lead")
    model = roll.model
    assert model is not None
    # C, C#, D#, F# over a C root.
    model.edit("Draw", add=model.new_notes([0, PPQ, 2 * PPQ, 3 * PPQ], [PPQ] * 4, [60, 61, 63, 66]))
    history = len(studio.project.history())

    def pitches() -> list[int]:
        notes = studio.project.notes("P1", "Lead")
        return [int(p) for p in notes["pitch"][notes["start"].argsort()]]

    # Lock off: choosing a scale highlights and edits nothing.
    roll.scale_box.setCurrentIndex(_scale_index(roll, "Major"))
    assert pitches() == [60, 61, 63, 66]
    assert len(studio.project.history()) == history
    # Lock on: its rising edge snaps, as one undo step.
    roll.lock_action.setChecked(True)
    assert pitches() == [60, 60, 62, 65]
    assert len(studio.project.history()) == history + 1
    assert studio.project.history()[-1].startswith("Snap notes to scale")
    # A scale change with Lock on re-snaps.
    roll.scale_box.setCurrentIndex(_scale_index(roll, "Minor pentatonic"))
    assert all(scales.in_scale(p, 0, scales.mask_of("Minor pentatonic")) for p in pitches())
    assert len(studio.project.history()) == history + 2
    # Already in the scale: no command.
    assert not roll.snap_notes_to_scale()
    assert len(studio.project.history()) == history + 2
    assert studio.project.undo()
    assert pitches() == [60, 60, 62, 65]


def test_custom_scale_reaches_the_combo(studio: MainWindow) -> None:
    roll = studio.piano_roll
    roll.custom_scales.save("Mine", 0b1001_0001)
    roll.set_custom_scales(roll.custom_scales)
    index = _scale_index(roll, "Mine")
    assert index > _scale_index(roll, "Pelog")
    assert roll.scale_box.itemText(roll.scale_box.count() - 1) == scales.CUSTOM_ENTRY
    roll.select_clip("P1", "Lead")
    roll.scale_box.setCurrentIndex(index)
    assert roll.model is not None
    assert roll.model.scale_mask == 0b1001_0001
    # It survives a restart: the main window's settings hold it.
    assert scales.CustomScales(studio.settings).mask("Mine") == 0b1001_0001


def test_custom_entry_edits_the_current_custom_scale(
    studio: MainWindow, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Choosing Custom… on a custom scale opens that scale (so it can be deleted); on a
    preset it opens a blank one."""
    roll = studio.piano_roll
    roll.custom_scales.save("Mine", 0b1001_0001)
    roll.set_custom_scales(roll.custom_scales)
    opened: list[str] = []

    def fake(name: str = "") -> str:
        opened.append(name)
        return scales.NO_SCALE if name else "Major"

    monkeypatch.setattr(roll, "edit_custom_scale", fake)
    custom = roll.scale_box.count() - 1
    roll.scale_box.setCurrentIndex(_scale_index(roll, "Dorian"))
    roll.scale_box.setCurrentIndex(custom)
    assert opened == [""]
    assert roll.scale_box.currentText() == "Major"
    roll.scale_box.setCurrentIndex(_scale_index(roll, "Mine"))
    roll.scale_box.setCurrentIndex(roll.scale_box.count() - 1)
    assert opened == ["", "Mine"]
    assert roll.scale_box.currentText() == scales.NO_SCALE
