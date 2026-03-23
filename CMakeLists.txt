cmake_minimum_required(VERSION 3.16)
project(AudioSequencer VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Platform-specific compile flags
if(APPLE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Wno-deprecated-declarations")
elseif(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4 /permissive-")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
endif()

include(FetchContent)

# --- 1. RtAudio ---
FetchContent_Declare(rtaudio
    GIT_REPOSITORY https://github.com/thestk/rtaudio.git
    GIT_TAG 6.0.1
)
set(RTAUDIO_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(rtaudio)

# --- 2. GLFW ---
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# --- 3. Dear ImGui ---
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.4-docking
)
FetchContent_MakeAvailable(imgui)
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
find_package(OpenGL REQUIRED)
target_link_libraries(ImGui PRIVATE OpenGL::GL)

# --- 4. ReaderWriterQueue ---
FetchContent_Declare(readerwriterqueue
    GIT_REPOSITORY https://github.com/cameron314/readerwriterqueue.git
    GIT_TAG v1.0.6
)
FetchContent_MakeAvailable(readerwriterqueue)
add_library(ReaderWriterQueue INTERFACE)
target_include_directories(ReaderWriterQueue INTERFACE ${readerwriterqueue_SOURCE_DIR})

# --- 5. Portable File Dialogs (THE FIX) ---
# We download the header directly to bypass their broken CMakeLists.txt and Git Tags
FetchContent_Declare(portable_file_dialogs
    URL https://raw.githubusercontent.com/samhocevar/portable-file-dialogs/master/portable-file-dialogs.h
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(portable_file_dialogs)
add_library(PortableFileDialogs INTERFACE)
# The file is downloaded as 'portable-file-dialogs.h' inside the _deps folder
target_include_directories(PortableFileDialogs INTERFACE ${portable_file_dialogs_SOURCE_DIR})

# --- Main Executable ---
add_executable(AudioSequencer
    src/main.cpp
    src/AudioEngine.cpp
    src/SequencerUI.cpp
    src/AdxParser.cpp
)
target_include_directories(AudioSequencer PRIVATE include)
target_link_libraries(AudioSequencer PRIVATE
    rtaudio glfw ImGui ReaderWriterQueue PortableFileDialogs
)

if(APPLE)
    find_library(COREAUDIO_FW CoreAudio)
    find_library(COREFOUNDATION_FW CoreFoundation)
    find_library(AUDIOUNIT_FW AudioUnit)
    find_library(COCOA_FW Cocoa)
    find_library(IOKIT_FW IOKit)
    target_link_libraries(AudioSequencer PRIVATE
        ${COREAUDIO_FW} ${COREFOUNDATION_FW} ${AUDIOUNIT_FW} ${COCOA_FW} ${IOKIT_FW}
    )
elseif(UNIX AND NOT APPLE)
    target_link_libraries(AudioSequencer PRIVATE asound pthread m dl)
endif()