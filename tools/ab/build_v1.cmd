@echo off
rem Builds the archived (iteration one) engine headless, for the Phase 4 A/B:
rem   v1render.exe  the archive exactly as it was
rem   v1fixed.exe   the same, minus two scheduling bugs (patch_v1.py says which) - the
rem                 reference the Tranche A gate measures against
rem Needs the archive's fetched dependencies under _archive\build\_deps (configure
rem _archive\src-cpp once), Python, and Visual Studio's x64 tools.
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%..\.."
set "SRC=%ROOT%\_archive\src-cpp"
set "DEPS=%ROOT%\_archive\build\_deps"
set "FLAGS=/nologo /std:c++20 /O2 /EHsc /MD /fp:precise /I"%DEPS%\rtaudio-src" /I"%DEPS%\readerwriterqueue-src""
set "COMMON="%HERE%v1render.cpp" "%HERE%stubs.cpp" "%SRC%\src\AudioEffects.cpp" "%SRC%\src\AdxParser.cpp" "%SRC%\src\PatternCompiler.cpp""
python "%HERE%patch_v1.py" || exit /b 1
if not exist "%TEMP%\adx_v1" mkdir "%TEMP%\adx_v1"
if not exist "%TEMP%\adx_v1fixed" mkdir "%TEMP%\adx_v1fixed"
call "%HERE%vs.cmd" cl %FLAGS% /I"%SRC%\include" %COMMON% "%SRC%\src\AudioEngine.cpp" ^
  /Fo"%TEMP%\adx_v1\\" /Fe"%HERE%v1render.exe" || exit /b 1
call "%HERE%vs.cmd" cl %FLAGS% /I"%HERE%fixed" /I"%SRC%\include" %COMMON% "%HERE%fixed\AudioEngine.cpp" ^
  /Fo"%TEMP%\adx_v1fixed\\" /Fe"%HERE%v1fixed.exe" || exit /b 1
