cmake_minimum_required(VERSION 3.16)
project(AudioSequencer VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Platform-specific compile flags
if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4 /WX /permissive-")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
endif()

include(FetchContent)

# --- 1. Fetch RtAudio ---
message(STATUS "Fetching RtAudio...")
FetchContent_Declare(
    rtaudio
    GIT_REPOSITORY https://github.com/thestk/rtaudio.git
    GIT_TAG        6.0.1
)
# We just need the static library
set(RTAUDIO_BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared RtAudio" FORCE)
set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "Build RtAudio tests" FORCE)
FetchContent_MakeAvailable(rtaudio)

# --- 2. Fetch GLFW ---
message(STATUS "Fetching GLFW...")
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# --- 3. Fetch Dear ImGui ---
message(STATUS "Fetching Dear ImGui...")
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.4-docking # Use docking branch for modern UI features
)
FetchContent_MakeAvailable(imgui)

# ImGui doesn't provide a CMakeLists.txt that builds a library out of the box easily
# We'll create a static library from its sources.
set(IMGUI_DIR ${imgui_SOURCE_DIR})
add_library(ImGui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(ImGui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(ImGui PRIVATE glfw)

# Find OpenGL for ImGui
find_package(OpenGL REQUIRED)
target_link_libraries(ImGui PRIVATE OpenGL::GL)

# --- 4. Fetch moodycamel::ReaderWriterQueue ---
message(STATUS "Fetching moodycamel::ReaderWriterQueue...")
FetchContent_Declare(
    readerwriterqueue
    GIT_REPOSITORY https://github.com/cameron314/readerwriterqueue.git
    GIT_TAG        v1.0.6
)
FetchContent_MakeAvailable(readerwriterqueue)

# ReaderWriterQueue is header-only
add_library(ReaderWriterQueue INTERFACE)
target_include_directories(ReaderWriterQueue INTERFACE ${readerwriterqueue_SOURCE_DIR})

# --- Main Executable ---
add_executable(AudioSequencer
    src/main.cpp
    src/AudioEngine.cpp
    src/SequencerUI.cpp
    src/AdxParser.cpp
)

target_include_directories(AudioSequencer PRIVATE include)

# Link dependencies
target_link_libraries(AudioSequencer PRIVATE
    rtaudio
    glfw
    ImGui
    ReaderWriterQueue
)

if(APPLE)
    # Required frameworks for RtAudio and GLFW on macOS
    find_library(COREAUDIO_FW CoreAudio)
    find_library(COREFOUNDATION_FW CoreFoundation)
    find_library(AUDIOUNIT_FW AudioUnit)
    find_library(COCOA_FW Cocoa)
    find_library(IOKIT_FW IOKit)
    target_link_libraries(AudioSequencer PRIVATE
        ${COREAUDIO_FW}
        ${COREFOUNDATION_FW}
        ${AUDIOUNIT_FW}
        ${COCOA_FW}
        ${IOKIT_FW}
    )
elseif(UNIX AND NOT APPLE)
    target_link_libraries(AudioSequencer PRIVATE asound pthread m dl)
endif()
