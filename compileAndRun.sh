#!/bin/bash

# Exit immediately if any command fails
set -e

# --- CONFIGURATION ---
# Matches the add_executable() target in your CMakeLists.txt
EXECUTABLE_NAME="AudioSequencer" 

echo "🛠️ Configuring project with CMake..."
mkdir -p build
cd build

# Generate the build system files
cmake ..

echo "⚙️ Compiling project..."
# Build the project (--parallel uses all available CPU cores)
cmake --build . --parallel

echo "✅ Build successful!"
echo "🚀 Running $EXECUTABLE_NAME..."
echo "--------------------------------------------------"

# --- SMART EXECUTION ---
# Handles both single-config (Linux/macOS Makefiles) and multi-config (Windows MSVC) layouts
if [ -f "./$EXECUTABLE_NAME" ]; then
    ./"$EXECUTABLE_NAME"
elif [ -f "./Debug/$EXECUTABLE_NAME.exe" ]; then
    ./Debug/"$EXECUTABLE_NAME.exe"
elif [ -f "./$EXECUTABLE_NAME.exe" ]; then
    ./"$EXECUTABLE_NAME.exe"
else
    echo "❌ Could not find the executable. Ensure it compiled correctly."
    exit 1
fi