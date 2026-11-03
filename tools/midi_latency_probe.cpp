// MIDI-in to audio-out latency, without a keyboard (P9-1's software half).
//
// Sends note-ons to a MIDI output port (a loopMIDI port that adX listens on) and
// captures the default render device in WASAPI loopback. Both sides are stamped on
// the QPC clock, so each note's latency is: first loud loopback frame - send time.
// That is everything up to the Windows mix. The device's own output buffer and DAC
// come after; the probe prints the endpoint's reported latency for that part.
//
// Build: cl /nologo /O2 /EHsc /std:c++20 midi_latency_probe.cpp
// Usage: midi_latency_probe "loopMIDI Port" [notes] [threshold]
// Prints one "note <i> <ms>" line per note, then "endpoint <ms>".

#define NOMINMAX
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")

namespace {

// QPC in 100 ns units, the unit WASAPI's packet timestamps use.
long long now100ns() {
    static const long long freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<long long>(static_cast<double>(t.QuadPart) * 1e7 / freq);
}

bool check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "error: %s failed (0x%08lx)\n", what, static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

int findPort(const std::string& wanted) {
    const UINT count = midiOutGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        MIDIOUTCAPSA caps{};
        if (midiOutGetDevCapsA(i, &caps, sizeof caps) == MMSYSERR_NOERROR &&
            std::string(caps.szPname).find(wanted) != std::string::npos) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

struct Capture {
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    UINT32 rate = 0;
    UINT32 channels = 0;
    bool isFloat = false;
    double endpointMs = 0.0;

    std::mutex lock;
    long long armedAt = 0;    // a note was sent at this time; look for its onset
    long long onsetAt = 0;    // the first loud frame after armedAt, 0 until found
    float peakSinceReset = 0; // loudest sample since the last resetPeak()
    float threshold = 0.01F;
    std::atomic<bool> running{true};

    void loop() {
        while (running) {
            UINT32 frames = 0;
            if (FAILED(capture->GetNextPacketSize(&frames))) {
                return;
            }
            while (frames > 0) {
                BYTE* data = nullptr;
                DWORD flags = 0;
                UINT64 position = 0;
                UINT64 qpc = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, &position, &qpc))) {
                    return;
                }
                scan(data, frames, flags, static_cast<long long>(qpc));
                capture->ReleaseBuffer(frames);
                if (FAILED(capture->GetNextPacketSize(&frames))) {
                    return;
                }
            }
            Sleep(1);
        }
    }

    void scan(const BYTE* data, UINT32 frames, DWORD flags, long long qpc) {
        const std::lock_guard<std::mutex> guard(lock);
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
            return;
        }
        for (UINT32 f = 0; f < frames; ++f) {
            float loudest = 0.0F;
            for (UINT32 c = 0; c < channels; ++c) {
                const std::size_t i = (static_cast<std::size_t>(f) * channels) + c;
                float v = 0.0F;
                if (isFloat) {
                    v = reinterpret_cast<const float*>(data)[i];
                } else {
                    v = static_cast<float>(reinterpret_cast<const short*>(data)[i]) / 32768.0F;
                }
                loudest = std::max(loudest, std::fabs(v));
            }
            peakSinceReset = std::max(peakSinceReset, loudest);
            const long long at = qpc + static_cast<long long>(f * 1e7 / rate);
            if (armedAt != 0 && onsetAt == 0 && at >= armedAt && loudest > threshold) {
                onsetAt = at;
            }
        }
    }
};

bool openLoopback(Capture& cap) {
    IMMDeviceEnumerator* enumerator = nullptr;
    if (!check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator)),
               "MMDeviceEnumerator")) {
        return false;
    }
    IMMDevice* device = nullptr;
    if (!check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device),
               "default render device")) {
        return false;
    }
    if (!check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&cap.client)),
               "IAudioClient")) {
        return false;
    }
    WAVEFORMATEX* format = nullptr;
    if (!check(cap.client->GetMixFormat(&format), "GetMixFormat")) {
        return false;
    }
    cap.rate = format->nSamplesPerSec;
    cap.channels = format->nChannels;
    cap.isFloat = format->wBitsPerSample == 32;
    std::fprintf(stderr, "mix format: %lu Hz, %u channels, %u bit\n", format->nSamplesPerSec,
                 format->nChannels, format->wBitsPerSample);
    if (!check(cap.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                      200000, 0, format, nullptr),
               "loopback Initialize")) {
        return false;
    }
    CoTaskMemFree(format);
    if (!check(cap.client->GetService(__uuidof(IAudioCaptureClient),
                                      reinterpret_cast<void**>(&cap.capture)),
               "IAudioCaptureClient")) {
        return false;
    }
    // What the render side adds after the mix: a shared-mode client's stream latency
    // on the same endpoint, plus its device period.
    IAudioClient* render = nullptr;
    if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(&render)))) {
        WAVEFORMATEX* mix = nullptr;
        REFERENCE_TIME period = 0;
        REFERENCE_TIME latency = 0;
        if (SUCCEEDED(render->GetMixFormat(&mix)) &&
            SUCCEEDED(render->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, mix, nullptr)) &&
            SUCCEEDED(render->GetStreamLatency(&latency)) &&
            SUCCEEDED(render->GetDevicePeriod(&period, nullptr))) {
            // Shared mode often reports 0 latency; the mix then waits one period.
            cap.endpointMs = static_cast<double>(std::max(latency, period)) / 1e4;
        }
        CoTaskMemFree(mix);
        render->Release();
    }
    // What a low-latency (IAudioClient3) shared stream gets on this device: the periods
    // the engine allows and the buffer it hands back at the smallest one.
    IAudioClient3* low = nullptr;
    if (SUCCEEDED(device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(&low)))) {
        WAVEFORMATEX* mix = nullptr;
        UINT32 def = 0;
        UINT32 fundamental = 0;
        UINT32 minimum = 0;
        UINT32 maximum = 0;
        UINT32 buffer = 0;
        if (SUCCEEDED(low->GetMixFormat(&mix)) &&
            SUCCEEDED(low->GetSharedModeEnginePeriod(mix, &def, &fundamental, &minimum,
                                                     &maximum)) &&
            SUCCEEDED(low->InitializeSharedAudioStream(0, minimum, mix, nullptr)) &&
            SUCCEEDED(low->GetBufferSize(&buffer))) {
            std::fprintf(stderr,
                         "engine periods (frames): default %u, fundamental %u, min %u, "
                         "max %u; buffer at min %u\n",
                         def, fundamental, minimum, maximum, buffer);
        }
        CoTaskMemFree(mix);
        low->Release();
    }
    device->Release();
    enumerator->Release();
    return check(cap.client->Start(), "loopback Start");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: midi_latency_probe <port name> [notes] [threshold]\n");
        return 2;
    }
    const std::string portName = argv[1];
    const int notes = argc > 2 ? std::atoi(argv[2]) : 30;
    if (!check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx")) {
        return 1;
    }
    Capture cap;
    if (argc > 3) {
        cap.threshold = static_cast<float>(std::atof(argv[3]));
    }
    if (portName == "--listen") {
        // Audio half only: the caller triggers the sound itself. Per note: print
        // "ready <qpc>" once quiet and armed, then "onset <qpc>" (100 ns units).
        if (!openLoopback(cap)) {
            return 1;
        }
        std::thread worker([&] { cap.loop(); });
        for (int i = 0; i < notes; ++i) {
            Sleep(400);
            long long armed = now100ns();
            {
                const std::lock_guard<std::mutex> guard(cap.lock);
                cap.onsetAt = 0;
                cap.armedAt = armed;
            }
            std::printf("ready %lld\n", armed);
            std::fflush(stdout);
            long long onset = 0;
            for (int waited = 0; waited < 2000 && onset == 0; ++waited) {
                Sleep(1);
                const std::lock_guard<std::mutex> guard(cap.lock);
                onset = cap.onsetAt;
            }
            std::printf("onset %lld\n", onset);
            std::fflush(stdout);
        }
        cap.running = false;
        worker.join();
        return 0;
    }
    const int port = findPort(portName);
    if (port < 0) {
        std::fprintf(stderr, "error: no MIDI output named like \"%s\"\n", portName.c_str());
        return 1;
    }
    HMIDIOUT out = nullptr;
    if (midiOutOpen(&out, static_cast<UINT>(port), 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        std::fprintf(stderr, "error: could not open MIDI output %d\n", port);
        return 1;
    }
    if (!openLoopback(cap)) {
        return 1;
    }
    std::thread worker([&] { cap.loop(); });
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    timeBeginPeriod(1);

    int failed = 0;
    for (int i = 0; i < notes; ++i) {
        // Wait for quiet: the last note's release and anything else playing.
        bool quiet = false;
        for (int tries = 0; tries < 40 && !quiet; ++tries) {
            {
                const std::lock_guard<std::mutex> guard(cap.lock);
                cap.peakSinceReset = 0.0F;
            }
            Sleep(50);
            const std::lock_guard<std::mutex> guard(cap.lock);
            quiet = cap.peakSinceReset < cap.threshold / 4.0F;
        }
        if (!quiet) {
            std::printf("note %d noisy\n", i);
            ++failed;
            continue;
        }
        // Jitter the send against the audio period so the samples spread over it.
        Sleep(static_cast<DWORD>(i * 7 % 11));
        const int pitch = 60 + (i % 12);
        {
            const std::lock_guard<std::mutex> guard(cap.lock);
            cap.onsetAt = 0;
            cap.armedAt = now100ns();
        }
        midiOutShortMsg(out, static_cast<DWORD>(0x90 | (pitch << 8) | (110 << 16)));
        long long onset = 0;
        for (int waited = 0; waited < 500 && onset == 0; ++waited) {
            Sleep(1);
            const std::lock_guard<std::mutex> guard(cap.lock);
            onset = cap.onsetAt;
        }
        midiOutShortMsg(out, static_cast<DWORD>(0x80 | (pitch << 8)));
        long long armed = 0;
        {
            const std::lock_guard<std::mutex> guard(cap.lock);
            armed = cap.armedAt;
            cap.armedAt = 0;
        }
        if (onset == 0) {
            std::printf("note %d silent\n", i);
            ++failed;
        } else {
            std::printf("note %d %.3f\n", i, static_cast<double>(onset - armed) / 1e4);
        }
        std::fflush(stdout);
        Sleep(150);
    }
    std::printf("endpoint %.3f\n", cap.endpointMs);
    cap.running = false;
    worker.join();
    timeEndPeriod(1);
    cap.client->Stop();
    midiOutReset(out);
    midiOutClose(out);
    return failed == notes ? 1 : 0;
}
