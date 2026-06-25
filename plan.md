# Project Implementation Plan: adX DAW & Hardstyle Remixer

## Agent Directives (Context for Cline / Qwen 14b)
**Project Context:** You are expanding `adX`, a custom C++20 audio sequencer/synthesizer, into a full Digital Audio Workstation (DAW) and automated Hardstyle Remix generator. 
**Host Environment:** The host system features 64GB DDR5 system RAM. You can comfortably utilize aggressive parallel compilation (`cmake --build . -j`) and safely load large ML models into memory without hitting OOM constraints.
**Architectural Strictness:** 1. **The Audio Thread is Sacred:** `AudioEngine::process` runs on a high-priority real-time thread. **DO NOT** introduce `std::mutex`, `std::string` allocations, memory allocations (`new`/`malloc`), or file I/O inside this loop. 
2. **Lock-Free Communication:** All state changes from the ImGui (Main) thread must be passed to the Audio thread via the existing `moodycamel::ReaderWriterQueue<AudioEvent>`.
3. **Heavy Processing:** File decoding, ONNX ML inference, and `.adx` parsing must happen on the Main Thread (or a background `std::thread`). 

---

## Phase 1: Digital Audio Playback & Arranger UI
**Goal:** Transition from a purely MIDI/Synth engine to an engine that can load and play raw PCM audio files (`.wav`, `.mp3`).

1. **Library Integration:** Update `CMakeLists.txt` to include `miniaudio` (a single-header audio library) via `FetchContent` or by directly downloading `miniaudio.h` into the `include/` directory.
2. **Data Structures:** * Create an `AudioClip` struct containing a `std::vector<float>` for PCM data, `sampleRate`, and `channels`.
    * Update the `Track` struct in `AudioData.h` to hold an `std::vector<AudioClip>` alongside the MIDI `notes`.
3. **AudioEngine Updates:** * In `AudioEngine::process`, iterate through active `AudioClip`s based on `m_currentSamplePosition`. Sum the PCM data into `sampleLeft` and `sampleRight` before the master compressor.
    * Ensure the lock-free garbage collection (`m_sequenceGarbageBin`) handles the safe destruction of old `AudioClip` data when tracks are updated.
4. **Arranger UI:** Update `SequencerUI.cpp` to support an "Arranger Mode." Draw horizontal bounding boxes representing `AudioClip`s on the timeline. 

---

## Phase 2: Live-Coding Hot Reload (Strudel-Style)
**Goal:** Allow users to edit `.adx` files in a text editor (like VS Code) and have the audio engine update seamlessly in real-time without stopping playback.

1. **File Watching:** Integrate a lightweight file watcher (e.g., `efsw` or a simple polling mechanism using `std::filesystem::last_write_time` on the main thread).
2. **Hot-Reload Logic:** * When `project.adx` changes, call `AdxParser::LoadProject` on the Main Thread into a temporary `SequencerState`.
    * Diff the temporary state against the current state.
    * For every changed `Track` or `Patch`, dispatch `AudioEventType::SequenceUpdate` or `AudioEventType::PatchUpdate` to the `ReaderWriterQueue`.
    * Ensure `AudioEngine` updates its active pointers without dropping audio frames.

---

## Phase 3: Phase Vocoder (Time-Stretching & Pitch-Shifting)
**Goal:** Implement the ability to pitch-up vocals and tune hardstyle kicks without altering their playback speed (and vice-versa).

1. **Library Integration:** Add the **RubberBand Library** (the industry standard C++ phase vocoder) to `CMakeLists.txt` via `FetchContent`.
2. **Parameter Expansion:** Add `pitchShiftSemitones` (float) and `timeStretchFactor` (float) to the `AudioClip` struct. Add corresponding ImGui sliders when clicking on an audio clip in the UI.
3. **DSP Implementation:** * Instantiate a `RubberBand::RubberBandStretcher` in `RealTime` mode for active audio tracks.
    * When `AudioEngine::process` pulls samples from an `AudioClip`, pass the raw PCM data through the `RubberBandStretcher` if the pitch/time parameters deviate from 1.0/0.0. 
    * *Agent Note:* Manage the latency/delay introduced by RubberBand carefully to keep the audio synced with the global `m_playheadPositionBeats`.

---

## Phase 4: Machine Learning Audio-to-MIDI (ONNX Runtime)
**Goal:** Allow the user to upload an original song and automatically extract the melody into an `.adx` track.

1. **Library Integration:** Add Microsoft's `ONNX Runtime` C++ API to `CMakeLists.txt`.
2. **Model Loading:** Implement a class `MelodyExtractor` that loads Spotify's `basic-pitch` ONNX model.
3. **Inference Pipeline (Main Thread Only):**
    * Downsample the input `AudioClip` to the rate expected by the model (usually 22.05kHz).
    * Convert the audio array into an `Ort::Value` tensor.
    * Run the inference session.
    * Parse the output tensor (a pitch contour matrix) into discrete MIDI `Note` events (Start Beat, Length, Pitch).
4. **ADX Generation:** Append the generated `Note`s to a new `Track` in the `SequencerState`, map it to a default synth `Patch`, and trigger a hot-reload sequence update.

---

## Phase 5: Hardstyle Remixer Workflow & DSP Effects
**Goal:** Finalize the remixer workflow by adding necessary audio effects and a sample browser.

1. **DSP Effects:** Implement an `AudioEffect` base class. 
    * Write a Freeverb or Dattorro based algorithmic `Reverb` (crucial for vocals and synth leads).
    * Write a simple Waveshaper/Distortion algorithm (crucial for hardstyle kicks).
    * Add `std::vector<std::unique_ptr<AudioEffect>>` to the `Track` struct. Update `AudioEngine::process` to route track audio through these effects before mixing.
2. **Sample Browser UI:** * Add a new ImGui window in `main.cpp` that lists `.wav` files from a `./samples` directory.
    * Implement ImGui drag-and-drop (`ImGui::BeginDragDropSource`) to allow dragging kick samples directly onto the `SequencerUI` timeline. 
    * Implement BPM auto-detection on imported samples so kicks automatically align with `snapBeat`.
3. **Hardstyle Auto-Generation (Optional Extension):** Create a macro button that populates a track with kicks on every downbeat (1, 1.5, 2, 2.5) matching the global `m_bpm`, and dynamically alters the `pitchShiftSemitones` of the kicks to follow the root notes of the ML-extracted melody from Phase 4.