@echo off
rem Runs a command with Visual Studio's x64 tools on PATH, from the repository root:
rem   tools\ab\vs.cmd cmake --build --preset windows-x64-debug
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0..\.."
%*
