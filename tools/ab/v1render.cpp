// Headless render of a v1 .adx through the ARCHIVED engine (iteration one), for the
// Phase 4 A/B reference. Mirrors ExportRenderer::Render: a fresh AudioEngine driven
// in 512-frame chunks, float32 WAV at kEngineSampleRate (44.1 kHz).
//
// usage: v1render <in.adx> <out.wav> [tail seconds] [solo track index]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "AdxParser.h"
#include "AudioEngine.h"

namespace {

void writeWav(const std::string& path, const std::vector<float>& data, unsigned rate) {
    std::ofstream out(path, std::ios::binary);
    const auto u32 = [&](unsigned v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](unsigned short v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const unsigned bytes = static_cast<unsigned>(data.size() * 4);
    out.write("RIFF", 4);
    u32(36 + bytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(3);
    u16(2);
    u32(rate);
    u32(rate * 8);
    u16(8);
    u16(32);
    out.write("data", 4);
    u32(bytes);
    out.write(reinterpret_cast<const char*>(data.data()), bytes);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: v1render in.adx out.wav [tail] [solo]\n";
        return 2;
    }
    SequencerState state;
    std::string firstPatch;
    if (!AdxParser::LoadProject(argv[1], state, firstPatch)) {
        std::cerr << "load failed\n";
        return 1;
    }
    const float tail = argc > 3 ? std::stof(argv[3]) : 4.0f;
    if (argc > 4) {
        const int solo = std::stoi(argv[4]);
        for (std::size_t t = 0; t < state.tracks.size(); ++t) {
            if (static_cast<int>(t) != solo) {
                state.tracks[t].volume = 0.0f;
            }
        }
        // v1 lets a mix.volume lane override a track's volume, so a muted track with
        // one still plays. Drop the other tracks' volume lanes.
        const std::string keep = state.tracks[static_cast<std::size_t>(solo)].patchName;
        std::erase_if(state.automation, [&](const AutomationLane& lane) {
            return lane.paramTarget == "mix.volume" && lane.trackTarget != keep;
        });
    }
    for (std::size_t t = 0; t < state.tracks.size(); ++t) {
        std::fprintf(stderr, "track %zu %s volume %.2f notes %zu effects %zu\n", t,
                     state.tracks[t].patchName.c_str(), state.tracks[t].volume,
                     state.tracks[t].notes.size(), state.tracks[t].effects.size());
    }
    float bpm = state.bpm.load();
    float lastBeat = 0.0f;
    for (const Track& track : state.tracks) {
        for (const auto& note : track.notes) {
            lastBeat = std::max(lastBeat, note.startBeat + note.lengthBeats);
        }
    }
    const float seconds = lastBeat * 60.0f / bpm + tail;
    const auto frames = static_cast<std::uint64_t>(seconds * kEngineSampleRate);

    moodycamel::ReaderWriterQueue<AudioEvent> queue(64);
    std::atomic<float> playhead{0.0f};
    AudioEngine engine(queue, kEngineSampleRate, playhead);
    AudioEvent evt{};
    evt.type = AudioEventType::SequenceUpdate;
    auto* snapshot = new SequenceSnapshot();
    snapshot->tracks = state.tracks;
    snapshot->patches = state.patches;
    snapshot->automation = state.automation;
    evt.data.sequence = snapshot;
    queue.enqueue(evt);
    evt = {};
    evt.type = AudioEventType::BpmChange;
    evt.data.bpmState.bpm = bpm;
    queue.enqueue(evt);
    evt = {};
    evt.type = AudioEventType::MasterVolChange;
    evt.data.masterVol.volume = state.masterVolume.load();
    queue.enqueue(evt);
    evt = {};
    evt.type = AudioEventType::GlobalTuningChange;
    evt.data.globalTuning.tuning = state.tuning.load();
    queue.enqueue(evt);
    const auto param = [&](EngineParam id, float value) {
        AudioEvent p{};
        p.type = AudioEventType::ParameterChange;
        p.data.paramData.paramId = static_cast<uint32_t>(id);
        p.data.paramData.value = value;
        queue.enqueue(p);
    };
    const MasterFxSettings& fx = state.masterFx;
    param(EngineParam::DelayTimeMs, fx.delayTimeMs);
    param(EngineParam::DelayFeedback, fx.delayFeedback);
    param(EngineParam::DelayMix, fx.delayMix);
    param(EngineParam::ReverbRoom, fx.reverbRoom);
    param(EngineParam::ReverbDamp, fx.reverbDamp);
    param(EngineParam::ReverbMix, fx.reverbMix);
    param(EngineParam::SidechainEnabled, std::getenv("V1_NODUCK") != nullptr ? 0.0f : fx.sidechainEnabled);
    param(EngineParam::SidechainAmount, fx.sidechainAmount);
    param(EngineParam::SidechainReleaseMs, fx.sidechainReleaseMs);
    param(EngineParam::MasterDrive, fx.masterDrive);
    evt = {};
    evt.type = AudioEventType::PlayStateChange;
    evt.data.playState.isPlaying = true;
    queue.enqueue(evt);

    std::vector<float> mix(frames * 2, 0.0f);
    for (std::uint64_t offset = 0; offset < frames; offset += 512) {
        const auto n = static_cast<unsigned>(std::min<std::uint64_t>(512, frames - offset));
        AudioEngine::audioCallback(mix.data() + offset * 2, nullptr, n, 0.0, 0, &engine);
    }
    writeWav(argv[2], mix, kEngineSampleRate);
    std::printf("rendered %llu frames (%.1f s) at %u Hz, master vol %.2f\n",
                static_cast<unsigned long long>(frames), seconds, kEngineSampleRate,
                state.masterVolume.load());
    return 0;
}
