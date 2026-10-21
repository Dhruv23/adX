"""Help > Quick start (F1): how to get around and make a first track, on one page.

Shown once on first launch (``MainWindow.maybe_show_quickstart``) and from the Help
menu. Every gesture it names is one the panels actually implement - keep it that way
when a gesture changes.
"""

from __future__ import annotations

from PySide6.QtWidgets import QDialog, QDialogButtonBox, QTextBrowser, QVBoxLayout, QWidget

GUIDE = """
<h2>adX quick start</h2>

<h3>Getting around</h3>
<ul>
<li><b>F5</b> Playlist &middot; <b>F6</b> Channel rack &middot; <b>F7</b> Piano roll
    &middot; <b>F8</b> Browser &middot; <b>F9</b> Mixer</li>
<li><b>F11</b> fills the window with the panel you are working in; F11 again puts the
    others back.</li>
<li>Drag the borders between panels to resize them. Drag a panel's title bar to move it.
    <i>View &gt; Reset layout</i> puts everything back.</li>
<li><b>Space</b> plays and pauses, <b>Ctrl+Z</b> undoes anything, <b>Ctrl+S</b> saves,
    <b>Ctrl+E</b> exports a WAV.</li>
</ul>

<h3>1. Make sounds - Channel rack (F6)</h3>
<ul>
<li>Pick an instrument in the toolbar and press <b>+ Channel</b>.</li>
<li>Click a step to add a note there; drag a lit step up or down to change its velocity;
    right-click a step to remove it.</li>
<li><b>Notes</b> opens the channel in the piano roll; the instrument button opens its
    editor on the right. Drop a preset from the Browser onto a channel to load it.</li>
</ul>

<h3>2. Write notes - Piano roll (F7)</h3>
<ul>
<li>With <b>Draw</b>: click to add a note, drag to set its length; right-click deletes.
    Use <b>Select</b> to drag notes around.</li>
<li>Mouse wheel scrolls pitch, <b>Shift</b>+wheel scrolls time, <b>Ctrl</b>+wheel zooms
    time, <b>Alt</b>+wheel zooms pitch. Click a key to hear it.</li>
</ul>

<h3>3. Arrange - Playlist (F5)</h3>
<ul>
<li>Choose a pattern in the toolbar, then click on a track to place it. Drag a clip to
    move it, drag its edge to resize it; right-click deletes it.</li>
<li>Double-click a pattern clip to edit it.</li>
</ul>

<h3>4. Mix - Mixer (F9)</h3>
<ul>
<li>A channel's <b>Output</b> (in the rack) picks its mixer insert; <b>+ Insert</b> adds
    one.</li>
<li>On a strip: <b>M</b> mute, <b>S</b> solo, the knobs pan and width, the fader the
    level. Click an empty effect slot to add an effect; click a full one to edit it.</li>
<li>Right-click any knob for <i>Create automation</i>; draw the curve in the Automation
    panel.</li>
</ul>
"""


class QuickStart(QDialog):
    """The guide, in a window that does not block the application."""

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("adX quick start")
        self.resize(640, 720)
        layout = QVBoxLayout(self)
        text = QTextBrowser()
        text.setHtml(GUIDE)
        layout.addWidget(text)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        buttons.rejected.connect(self.close)
        layout.addWidget(buttons)
