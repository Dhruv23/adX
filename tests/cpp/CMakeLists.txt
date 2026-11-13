adx_add_cpp_test(adx_tests
    SOURCES
        test_smoke.cpp
        CrtReport.cpp
        dsp/test_convolve.cpp
        dsp/test_envelope.cpp
        dsp/test_fft.cpp
        dsp/test_filters.cpp
        dsp/test_math.cpp
        dsp/test_noise.cpp
        dsp/test_oscillator.cpp
        dsp/test_resample.cpp
        dsp/test_small_primitives.cpp
        dsp/test_loudness.cpp
        effects/EffectHarness.cpp
        effects/test_effect_base.cpp
        effects/test_tranche_c.cpp
        effects/test_tranche_d.cpp
        effects/test_tranche_e.cpp
        physical/test_physical.cpp
        instruments/InstrumentHarness.cpp
        instruments/test_instrument_base.cpp
        instruments/test_instrument_cost.cpp
        instruments/test_sampler.cpp
        instruments/test_tranche_b.cpp
        instruments/test_wavetable_extra.cpp
        instruments/test_drum_hats.cpp
        instruments/test_va.cpp
        instruments/test_voice.cpp
        instruments/VoicebankFixture.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../local/teto/test_teto.cpp
        preset/test_presets.cpp
        rack/test_rack.cpp
        modmatrix/test_modmatrix.cpp
        sfz/test_sfz.cpp
        soundfont/test_soundfont.cpp
        gm/test_gm_parts.cpp
        gm/test_gm_articulation.cpp
        gm/test_gm_reference.cpp
        rt/test_alloc_guard.cpp
        rt/test_spsc_ring.cpp
        rt/test_overwrite_ring.cpp
        rt/test_fixed_containers.cpp
        rt/test_block_arena.cpp
        rt/test_reaper.cpp
        rt/test_stream_silence.cpp
        rt/test_wasapi_exclusive.cpp
        audio/test_capture.cpp
        project/test_time.cpp
        project/test_tempomap.cpp
        project/test_curve.cpp
        project/test_commands.cpp
        project/RandomCommands.cpp
        project/test_undo_random.cpp
        project/test_edit_notes.cpp
        project/test_validate.cpp
        project/test_phase6_commands.cpp
        geometry/test_geometry_buffer.cpp
        geometry/test_hittest.cpp
        geometry/test_peak_cache.cpp
        geometry/bench_piano_roll.cpp
        geometry/test_phase6_geometry.cpp
        geometry/test_analyzers.cpp
        geometry/test_bar_start.cpp
        midi/test_sysex.cpp
        dx7/MsfaOracle.cpp
        dx7/test_engine_vs_msfa.cpp
        dx7/test_codec.cpp
        dx7/test_dx7_instrument.cpp
        effects/test_fm1_effects.cpp
        instruments/test_midiout.cpp
        instruments/test_drum_models.cpp
        format/test_lexer.cpp
        format/test_diagnostics.cpp
        format/test_roundtrip.cpp
        format/test_parser.cpp
        format/test_writer.cpp
        format/test_v1shim.cpp
        format/test_note_extensions.cpp
        format/test_zones.cpp
        format/test_decode.cpp
        graph/GraphFixtures.cpp
        graph/test_buffers.cpp
        graph/test_cycle.cpp
        graph/test_pdc.cpp
        graph/test_routing.cpp
        graph/test_scheduler.cpp
        graph/test_toposort.cpp
        graph/test_voice_identity.cpp
        graph/test_glides_automation.cpp
        transport/test_loop.cpp
        transport/test_seek.cpp
        transport/test_timesource.cpp
        transport/test_two_sources.cpp
        session/test_spike.cpp
        session/test_state_machine.cpp
        session/test_session_format.cpp
        session/test_session_engine.cpp
        session/test_release.cpp
        session/test_recorder.cpp
        render/GoldenHashes.cpp
        render/RenderEvidence.cpp
        render/RenderFixtures.cpp
        render/test_bit_identity.cpp
        render/test_golden_units.cpp
        render/test_golden_racks.cpp
        render/test_golden_gate.cpp
        render/test_corpus_render.cpp
        render/test_spectral_match.cpp
        render/test_parallel_render.cpp
        render/test_pdc_effects.cpp
        render/test_decode_async.cpp
        render/test_levels.cpp
        render/test_no_alloc_under_load.cpp
        mini/test_compiler.cpp
        sync/test_patch.cpp
        sync/test_reload.cpp
        sync/test_sync.cpp
        sync/test_watcher.cpp
        sync/test_incremental.cpp
        render/test_snapshot.cpp
        render/test_phase6_engine.cpp
        clip/ClipFixtures.cpp
        clip/test_audio_clips.cpp
        clip/test_clip_edits.cpp
        render/test_export.cpp
        render/test_bundle.cpp
        render/test_golden_formats.cpp
        midi/test_midi.cpp
        analysis/test_analysis.cpp
        analysis/test_harmony.cpp
        analysis/test_timbre_cleaner.cpp
        analysis/test_melody.cpp
        generate/test_generate.cpp
        record/test_record.cpp
        midi/test_midi_device.cpp
        midi/test_learn.cpp
        midi/test_midi_sync.cpp
        midi/test_midi_bend.cpp
        midi/test_midi_take.cpp
        midi/test_pads.cpp
        plugin/PluginFixtures.cpp
        plugin/test_protocol.cpp
        plugin/test_cache.cpp
        plugin/test_parammap.cpp
        plugin/test_scanner.cpp
        plugin/test_watchdog.cpp
        plugin/test_plugin_render.cpp
        plugin/test_plugin_automation.cpp
)

# The corpus in docs/examples/ is a permanent parser regression fixture
# (FINAL_PLAN §6), and docs/adx-format-v2.md is what the diagnostic-code check reads.
# Both are found relative to the source tree rather than the build tree, because the
# test binary is run from wherever ctest happens to put it.
target_compile_definitions(adx_tests PRIVATE ADX_REPO_ROOT="${CMAKE_SOURCE_DIR}")

# Phase 11.5: unmodified upstream msfa, the dx7 port's bit-exactness oracle.
target_link_libraries(adx_tests PRIVATE adx_msfa_upstream)
target_link_libraries(adx_tests PRIVATE adx_tsf)

# Phase 9: the sandbox child and the fixture plugins the plugin tests drive
# (phase_9.md §4.8). Built before the tests, found by absolute path.
add_subdirectory(${CMAKE_SOURCE_DIR}/tests/fixtures/plugins ${CMAKE_BINARY_DIR}/fixtures/plugins)
add_dependencies(adx_tests adx_plugin_host adx_test_plugin_clap adx_test_plugin_vst3)
target_compile_definitions(adx_tests PRIVATE
    ADX_PLUGIN_HOST_EXE="$<TARGET_FILE:adx_plugin_host>"
    ADX_TEST_PLUGIN_CLAP="$<TARGET_FILE:adx_test_plugin_clap>"
    ADX_TEST_PLUGIN_VST3="$<TARGET_FILE:adx_test_plugin_vst3>")

# With ADX_ENABLE_ONNX the runtime DLL has to sit beside the test binary, and before
# test discovery runs it, so it is copied at configure time rather than after the link.
if(ADX_ENABLE_ONNX)
    file(COPY "${ADX_ONNX_RUNTIME_DLL}" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
endif()
