target_sources(adx_engine_static PRIVATE
    Factory.cpp
    additive/AdditiveInstrument.cpp
    additive/AdditiveParams.cpp
    drumsynth/DrumSynth.cpp
    drumsynth/HardstyleKick.cpp
    fm/FmInstrument.cpp
    fm/FmParams.cpp
    granular/GranularInstrument.cpp
    pool/SamplePoolChannel.cpp
    sampler/SamplerInstrument.cpp
    sampler/SamplerSetup.cpp
    slicer/Slicer.cpp
    va/VaInstrument.cpp
    voice/UtauResampler.cpp
    voice/Voicebank.cpp
    voice/VoiceInstrument.cpp
    voice/VoiceRenderCache.cpp
    voice/VoiceSetup.cpp
    voice/WorldAnalysis.cpp
    wavetable/WavetableInstrument.cpp
    wavetable/WavetableSetup.cpp
)
# WORLD: the Voice instrument's analysis and resynthesis, on worker threads only.
target_link_libraries(adx_engine_static PRIVATE adx_world)
