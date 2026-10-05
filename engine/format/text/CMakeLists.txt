# Text decoding the formats need: Shift-JIS for UTAU voicebanks (phase_4.md §4.13).
target_sources(adx_engine_static PRIVATE
    ShiftJis.cpp
    ShiftJisTable.cpp
)
