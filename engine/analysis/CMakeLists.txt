target_sources(adx_engine_static PRIVATE
    AnalysisJob.cpp
    AudioInput.cpp
    BpmDetector.cpp
    Chroma.cpp
    ChordDetector.cpp
    ChordSuggest.cpp
    KeyDetector.cpp
    NoteCreation.cpp
    OnsetDetector.cpp
    TimbreAnalyzer.cpp
    TrackCleaner.cpp
)

# The ONNX half of melody extraction exists only with ADX_ENABLE_ONNX (phase_10.md §4.2):
# off, the extractor is not compiled at all and nothing links the runtime.
if(ADX_ENABLE_ONNX)
    target_sources(adx_engine_static PRIVATE MelodyExtractor.cpp)
    target_link_libraries(adx_engine_static PUBLIC adx_onnxruntime)
    target_compile_definitions(adx_engine_static PUBLIC ADX_ONNX_MODEL="${ADX_ONNX_MODEL}")
endif()
