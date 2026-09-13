// Single translation unit providing the miniaudio implementation.
// Device I/O, the ma_engine node graph, and the resource manager are disabled
// since RtAudio already owns the audio device and AudioEngine::process() does
// its own manual mixing — this file is decode-only (WAV/MP3 via ma_decoder).
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DEVICE_IO
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#include "miniaudio.h"
