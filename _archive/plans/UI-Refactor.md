# UI Refactor Plan: FL-Studio-style Dark DAW Interface

**Goal:** Bring adX's ImGui front-end to visual parity with the reference
screenshots (a black/white FL Studio arrangement view — icon transport bar,
white-on-black waveform-filled track lanes, a live spectrum line, a
particle-style transient visualizer, and a receding-line spectrogram
waterfall). This is a **UI-thread-only** refactor: no audio-thread
architecture changes. `AudioEngine::process` stays real-time-safe; the only
new engine-side work is a lock-free read tap for visualization data, added
the same way `moodycamel::ReaderWriterQueue<AudioEvent>` already crosses the
thread boundary. Engine/DSP feature work (new oscillators, effects, etc.)
belongs to `plan.md`, not here.

**Current state (baseline):**
- `SetupImGuiStyle()` (`src/main.cpp:44`) already sets pure-black
  window/child/frame backgrounds, white text, zero rounding/borders — the
  monochrome foundation is in place.
- The transport bar (`src/main.cpp:945-1081`) is stock ImGui `Button`/
  `SliderFloat`/`InputText` widgets in a single row — functional, not styled.
- Docking is already wired up (`BuildDefaultDockLayout`, `src/main.cpp:626`;
  `ImGuiConfigFlags_DockingEnable`, `src/main.cpp:752`) with panels for
  SAMPLE BROWSER / PATCH SUITE / MASTER FX / TRACK FX / EXPORT / PATCH EDITOR
  / SEQUENCER.
- Arranger mode (`SequencerUI.cpp:377-484`, toggled via the "Arranger" radio
  at `SequencerUI.cpp:104`) already draws audio clips as track lanes — but as
  flat solid-color rectangles with a filename label
  (`SequencerUI.cpp:480-482`), not waveforms. This is the closest existing
  analog to the reference's playlist view.
- No FFT, no spectrum/analyzer panel, no particle or waterfall visualizer
  exist anywhere in the repo today (confirmed via grep for `FFT`/`Spectrum`/
  `Visualizer`/`Waveform`) — these are new, purely additive panels.

---

## Phase 1: Theme & Chrome Pass
**Goal:** Tighten the existing black/white theme from "flat ImGui defaults"
to the reference's compact, dense DAW chrome.

1. Extend `SetupImGuiStyle()` (`src/main.cpp:44`): shrink `FramePadding`
   (~4,2) and `ItemSpacing` (~6,4) for a denser toolbar; set `FrameRounding`
   to ~2px on buttons only (reference toolbar buttons are subtly pill-shaped,
   everything else — clips, panels — stays hard-edged); add a thin
   (~1px, low-alpha gray) `ImGuiCol_Border`/`ImGuiCol_Separator` so track
   rows and panels get the faint hairline dividers visible in the
   screenshots, instead of today's zero-border look.
2. Load a monospace font (e.g. bundle a small TTF via `ImGui::GetIO().Fonts`)
   for numeric readouts — bar:beat:tick counter, BPM, CPU/RAM — matching the
   reference's tabular-numeral time display top-left.
3. Replace the text-button transport bar (`src/main.cpp:945-1081`) with a
   compact icon cluster: small fixed-size square buttons for Play/Stop/
   Record/Loop (glyphs from an icon font, e.g. a bundled subset of Forkawesome/
   Material icons, rather than the current "PLAY"/"STOP" text labels), then
   the monospace time readout, then the existing BPM/file controls
   right-aligned as today. Existing LOAD/SAVE/EXTRACT MELODY/IMPORT MIDI
   actions are kept, just re-skinned as small icon+label buttons.
4. Restyle track header rows in the Arranger (`SequencerUI.cpp`, gutter draw
   at `SequencerUI.cpp:637-656`) from the current tab-bar-per-track model
   toward the reference's thin single-line rows (small disclosure caret +
   track name + "Mix level" sub-label), while keeping the tab bar's existing
   add/close/select behavior underneath — this is a visual re-skin of
   `BeginTabItem` row rendering, not a data-model change.

**Acceptance:** Transport bar and track headers read as dense/monochrome
icon-first chrome, not default ImGui widgets; no behavior changes.

---

## Phase 2: Real Waveform Rendering for Audio Clips
**Goal:** Replace the solid-color clip rectangles in Arranger mode with the
white-on-black waveform fills seen in every reference screenshot.

1. **Peak cache:** Add a small cache keyed by `(filePath, pitchShiftSemitones,
   timeStretchFactor, reversed)` — the same identity `AudioClip::operator==`
   already uses (`AudioData.h:204-208`) — that stores precomputed min/max
   peak pairs per pixel-column at a few zoom tiers. Build it lazily off
   `AudioClip::pcmData` on the Main Thread whenever a clip is added or
   `AudioClipProcessor::ReprocessClip` runs (both already Main-Thread-only
   operations); never touched from the audio thread.
2. **Draw:** In the clip-drawing loop (`SequencerUI.cpp:466-484`), replace
   `AddRectFilled`/`AddText` with: a black clip background, a thin white
   polyline (or per-column vertical tick pair) tracing the min/max envelope
   vertically centered in the clip rect, and a slim lighter-gray title strip
   across the top holding the filename — matching the reference's clip look
   exactly (e.g. the "Fairy Spring..." and "Kickstart 2..." lanes).
3. **Collapsed "Mix level" preview:** Track header rows (Phase 1) get a
   1-line condensed rendering of the same peak data behind/beside the track
   name, matching the reference's thin waveform sliver next to each track
   label even when the full lane isn't expanded.
4. Selection/hover states (`COLOR_CLIP_HOVERED`, `SequencerUI.cpp:22`) still
   apply as a tint/outline over the waveform rather than a solid fill swap.

**Acceptance:** Arranger-mode audio clips visually match the reference —
recognizable waveform shapes, not flat blocks — including for the currently
loaded `suffocation.adx` clips.

---

## Phase 3: Master Output Tap + Spectrum Analyzer Panel
**Goal:** A live frequency-domain readout (the smoothed white line plot
bottom-middle of the reference) — needs a way to get audio data to the UI
thread and a way to turn it into a spectrum.

1. **Lock-free tap:** Add a small SPSC ring buffer (same
   `moodycamel`-style lock-free structure already used for `AudioEvent`) that
   `AudioEngine::process` writes the post-mix master samples into every
   block — a plain fixed-size ring write, no allocation, no blocking; this is
   the only audio-thread-adjacent change in this whole plan, and it's
   strictly additive (a write that can be dropped/overwritten if the UI
   thread falls behind, never a wait).
2. **FFT:** Add one small self-contained radix-2 FFT (no new third-party
   dependency — a ~50-line header, since none exists in the repo yet per the
   grep above). Windows (Hann) the last N tapped samples on the UI thread,
   transforms, takes log-magnitude per bin.
3. **Panel:** New dockable `"SPECTRUM"` window (added to
   `BuildDefaultDockLayout`, `src/main.cpp:626`) rendering log-magnitude bins
   as a filled white polyline on black, log-scaled frequency axis, with a
   light smoothing/peak-hold filter across frames so it reads as the
   reference's smooth-but-jagged trace rather than a jittery raw FFT.

**Acceptance:** SPECTRUM panel shows a live, smoothly-updating frequency
trace that visibly reacts to playback (bass hits move the low end, hats
move the high end).

---

## Phase 4: Transient Particle Visualizer
**Goal:** The purple/cyan particle "fountain" reacting to transients
(bottom-left of the reference).

1. **Onset/transient detection:** Reuse Phase 3's tapped ring buffer; compute
   short-window RMS on the UI thread each frame and flag a transient when it
   jumps above a decaying envelope follower (simple, cheap, no FFT needed for
   this part).
2. **Particle pool:** Fixed-size (e.g. 512) pre-allocated particle array —
   no per-frame allocation. On a transient, spawn a burst from a fixed origin
   point with velocity/spread scaled to transient strength; every frame,
   integrate position, fade alpha, recycle dead particles back into the pool.
3. **Panel:** New dockable `"VISUALIZER"` window drawing the pool via
   `ImGui::GetWindowDrawList()->AddCircleFilled`, colored along the
   reference's purple→cyan gradient by particle age.

**Acceptance:** Particle burst visibly fires in sync with kicks/transients
during playback; idle/silence settles to no new particles.

---

## Phase 5: Spectrogram Waterfall
**Goal:** The receding-line waterfall (bottom-right of the reference) —
frequency history over time, drawn in a pseudo-3D line-stack rather than a
flat heatmap image, matching what the screenshots actually show.

1. Keep a small ring buffer of the last M frames of Phase 3's log-magnitude
   bins (M ≈ 24-32, one column per rendered frame at a throttled rate, e.g.
   every 3rd UI frame, so the history spans a few seconds).
2. **Panel:** New dockable `"WATERFALL"` window: draw M overlaid polylines,
   each older frame offset progressively up-and-over and dimmed (decreasing
   brightness/increasing y-offset per step back), reproducing the receding
   layered-line look in the reference without any texture upload — pure
   `AddPolyline` calls over the cached bin history.

**Acceptance:** WATERFALL panel shows several overlaid frequency traces
receding "into" the panel, visibly scrolling forward as playback continues.

---

## Phase 6: Ruler, Layout Wiring & Final Polish Pass
**Goal:** Tie the arrangement ruler and the three new panels into the
existing dock layout, then verify against the references directly.

1. Add a bar:beat ruler strip across the top of the Arranger grid
   (`SequencerUI.cpp`, above `gridPos`/`gridSize` at `SequencerUI.cpp:208-209`)
   with tick marks every bar and a running time readout, matching the
   reference's "0:08:38 / bar" header over the playlist.
2. Wire `"SPECTRUM"`, `"VISUALIZER"`, `"WATERFALL"` into
   `BuildDefaultDockLayout` (`src/main.cpp:626-644`) as a bottom-docked row
   spanning the window, mirroring the reference's bottom strip
   (particle fountain | spectrum/waveform | waterfall, left to right).
3. Screenshot the running app's Arranger + bottom panel row side-by-side
   against all four reference images; adjust spacing/line-weight/font-size
   until the match is close (exact pixel parity isn't the bar — silhouette,
   density, and monochrome palette are).

**Acceptance:** A single screenshot of adX's main window, next to the
reference screenshots, reads as "the same visual language" — same toolbar
density, same waveform-filled black track lanes, same bottom-row trio of
spectrum/particles/waterfall.
