# Third-party software in adX

Generated from `cmake/AdxDependencies.cmake` by `tools/gen_third_party.py`; edit the
`# adx-license:` lines there, not this file. Each project's own licence text is in
its source, under the build's `_deps/` folder.

**Rubber Band** is GPL-2.0-or-later: an adX binary that links it is distributed under
the GPL (FINAL_PLAN §8). **shine** is LGPL and linked statically: on request, the object
files needed to relink adX against a modified shine are provided.

## Engine (C++)

| Project | Version | Licence | Used for |
|---|---|---|---|
| pybind11 | v2.13.6 | BSD-3-Clause | bindings |
| RtAudio | 6.0.1 | MIT | audio devices |
| Catch2 | v3.7.1 | BSL-1.0 | tests only, not shipped |
| RtMidi | 6.0.0 | MIT | MIDI devices |
| miniaudio | 0.11.25 | MIT-0 | audio decoding |
| WORLD | d625e7608ca23a870018f01e7c562ac683d9847f | BSD-3-Clause | voice synthesis |
| Rubber Band Library | v4.0.0 | GPL-2.0-or-later | time-stretch; makes a distributed adX binary GPL |
| shine | 3.1.1 | LGPL-2.0 | MP3 encoding; linked statically, relinkable on request |
| libFLAC | 1.4.3 | BSD-3-Clause | FLAC |
| libogg | v1.3.5 | BSD-3-Clause | Ogg |
| libvorbis | v1.3.7 | BSD-3-Clause | Vorbis |
| CLAP | 1.2.6 | MIT | plugin hosting |
| VST 3 SDK interfaces | v3.8.0_build_66 | MIT | plugin hosting |
| VST 3 public SDK | v3.8.0_build_66 | MIT | plugin hosting |
| ONNX Runtime | 1.27.1 | MIT | optional melody extractor |
| Basic Pitch model | 0.4.0 | Apache-2.0 | optional melody extractor |

## Runtime (Python)

| Project | Version | Licence | Used for |
|---|---|---|---|
| Python 3.12 (embeddable) | 3.12 | PSF-2.0 | the interpreter the app runs on |
| PySide6 (Qt for Python) | 6.11.2 | LGPL-3.0-only | the user interface; shipped as separate DLLs |
| NumPy | >=1.26 | BSD-3-Clause | arrays across the engine boundary |
