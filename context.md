# adX Project Context & State

## Project Overview
This file maintains the live state of the `adX` DAW and Hardstyle Remixer project. AI agents must read this file to understand the existing architecture before modifying code, and must update the "Implementation Status" section upon completing any phase outlined in `plan.md`.

## Repository Structure
The project is a C++ application utilizing a CMake build system[cite: 1]. The current baseline repository structure includes:
* **Root Configuration & Scripts**: `CMakeLists.txt`, `.gitignore`, `compileAndRun.sh`, `dev.sh`[cite: 1].
* **Documentation & Data**: `README.md`, `adxFormat.md`, `example.adx`[cite: 1].
* **Headers (`include/`)**: `AdxParser.h`, `AudioData.h`, `AudioEngine.h`, `SequencerUI.h`[cite: 1].
* **Source Code (`src/`)**: `AdxParser.cpp`, `AudioEngine.cpp`, `SequencerUI.cpp`, `main.cpp`[cite: 1].

## Core Components Overview
* **AudioEngine (`AudioEngine.h`, `AudioEngine.cpp`)**: Manages the real-time audio thread[cite: 1]. Currently functions as a lock-free additive synthesizer but needs to be expanded to handle raw PCM data playback and DSP effects.
* **AudioData (`AudioData.h`)**: Defines the internal state and foundational data structures (e.g., MIDI notes, synth patches)[cite: 1]. Needs to be updated to support `AudioClip` rendering.
* **AdxParser (`AdxParser.h`, `AdxParser.cpp`)**: Parses the custom `.adx` project format into internal sequencer states[cite: 1]. 
* **User Interface (`SequencerUI.h`, `SequencerUI.cpp`, `main.cpp`)**: Provides the graphical frontend[cite: 1]. Currently an ImGui-based piano roll that will need to accommodate a multitrack Arranger View and a Sample Browser.

## Implementation Status
*Agent Directive: Update the checkboxes `[ ]` to `[x]` upon completion of a phase. Add brief bullet points under completed phases noting any specific libraries integrated or architectural decisions made during execution.*

* [x] **Baseline Status**: C++ CMake project established, basic additive synth engine built, ImGui piano roll integrated, custom `.adx` text parsing functional[cite: 1].
* [ ] **Phase 1: Digital Audio Playback & Arranger UI**
    * *Status*: Pending
    * *Notes*: Awaiting integration of `miniaudio` and `AudioClip` data structures.
* [ ] **Phase 2: Live-Coding Hot Reload (Strudel-Style)**
    * *Status*: Pending
    * *Notes*: Awaiting file-watching system to trigger `AdxParser` re-evaluation and lock-free thread updates.
* [ ] **Phase 3: Phase Vocoder (Time-Stretching & Pitch-Shifting)**
    * *Status*: Pending
    * *Notes*: Awaiting `RubberBand` library integration for independent pitch and time control.
* [ ] **Phase 4: Machine Learning Audio-to-MIDI**
    * *Status*: Pending
    * *Notes*: Awaiting `ONNX Runtime` integration and melody extraction pipeline via Main Thread.
* [ ] **Phase 5: Hardstyle Remixer Workflow & DSP Effects**
    * *Status*: Pending
    * *Notes*: Awaiting custom DSP (Reverb/Distortion), Sample Library UI, and kick-drum auto-generation logic.