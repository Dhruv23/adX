@echo off
setlocal enabledelayedexpansion

SET EXECUTABLE_NAME=AudioSequencer

echo 🛠️ Configuring project with CMake...

:: Clear any conflicting Makefile caches from old environment errors
if exist build\CMakeCache.txt (
    echo Found old or conflicting CMake cache. Performing a clean configuration...
    rmdir /s /q build
)

if not exist build (
    mkdir build
)

cd build

:: Target your exact Visual Studio 2026 suite automatically
cmake -G "Visual Studio 18 2026" -A x64 ..
if %ERRORLEVEL% neq 0 (
    echo ❌ CMake configuration failed.
    cd ..
    exit /b %ERRORLEVEL%
)

echo ⚙️ Compiling project...
:: Build using all available CPU cores natively
cmake --build . --config Debug --parallel
if %ERRORLEVEL% neq 0 (
    echo ❌ Compilation failed!
    cd ..
    exit /b %ERRORLEVEL%
)

echo ✅ Build successful!
echo 🚀 Running %EXECUTABLE_NAME%...
echo --------------------------------------------------

:: Execute from the Visual Studio multi-configuration output directory
if exist ".\Debug\%EXECUTABLE_NAME%.exe" (
    :: 'start ""' launches the exe in a new process, allowing this script to exit
    start "" ".\Debug\%EXECUTABLE_NAME%.exe"
) else (
    echo ❌ Could not find the executable at .\Debug\%EXECUTABLE_NAME%.exe
    cd ..
    exit /b 1
)

cd ..