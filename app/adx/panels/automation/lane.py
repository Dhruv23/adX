"""Automation lanes as the panels make them (phase_6.md 4.5).

"Keyframing from the control": right-click any automatable control, "Create automation",
and one undo step makes a playlist automation clip targeting that parameter, starting
with a point at the parameter's current value, placed on the "Automation" track (made
if there is none) and holding its last value to the end of the arrangement.
"""

from __future__ import annotations

import numpy as np

from adx.session import Session

AUTOMATION_TRACK = "Automation"
#: Lane colours, by order of creation (the knob's ring takes its lane's).
LANE_COLORS = ("#ff78a0", "#78c896", "#ffc45c", "#a08cff", "#5fb2f0", "#ff9a5c")


def automation_track(session: Session) -> int:
    """The id of the track automation clips go on, made if missing (inside the caller's
    group)."""
    project = session.require()
    for track in project.playlist_info()["tracks"]:
        if str(track["name"]) == AUTOMATION_TRACK:
            return int(track["id"])
    session.build_and_execute("AddPlaylistTrackNew", AUTOMATION_TRACK)
    return max(int(t["id"]) for t in project.playlist_info()["tracks"])


def create_lane(session: Session, path: str, at: int = 0) -> int:
    """A lane on ``path`` starting at its current value: one undo step. Returns the
    automation clip's id, or 0 when the path names nothing."""
    project = session.require()
    value = project.param_value(path)
    if value is None:
        session.failed.emit("automate", f"{path} does not name a parameter")
        return 0
    with session.group(f"Automate {path}"):
        track = automation_track(session)
        points = np.array([[float(at), value, 0.0, 0.0]], dtype=np.float64)
        session.build_and_execute("AddAutomationClip", 0, path, points)
        clip = max(int(c["id"]) for c in project.automation_clips())
        session.build_and_execute("AddAutomationItem", track, clip, 0, 0)
    return clip


def automated_paths(session: Session) -> dict[str, str]:
    """Every automated path, with its lane's colour: one read."""
    if session.project is None:
        return {}
    out: dict[str, str] = {}
    for index, clip in enumerate(session.project.automation_clips()):
        if bool(clip["valid"]):
            out.setdefault(str(clip["target"]), LANE_COLORS[index % len(LANE_COLORS)])
    for controller in session.project.controllers_info():
        if bool(controller["valid"]):
            out.setdefault(str(controller["target"]), LANE_COLORS[0])
    return out
