cmake_minimum_required(VERSION 3.18)
project(LiberaTimecode VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LIBERA_TIMECODE_APP_VERSION "${PROJECT_VERSION}")
set(LIBERA_TIMECODE_APP_BUNDLE_NAME "Libera Timecode")

set(EXTERN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern")

# ---------------- Vendored dependencies ----------------

# GLFW (uses its own CMakeLists in extern/glfw)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${EXTERN_DIR}/glfw" EXCLUDE_FROM_ALL)

# Dear ImGui — compile as static lib (no upstream CMake)
add_library(libera_timecode_imgui STATIC
    "${EXTERN_DIR}/imgui/imgui.cpp"
    "${EXTERN_DIR}/imgui/imgui_draw.cpp"
    "${EXTERN_DIR}/imgui/imgui_tables.cpp"
    "${EXTERN_DIR}/imgui/imgui_widgets.cpp"
    "${EXTERN_DIR}/imgui/imgui_demo.cpp"
    "${EXTERN_DIR}/imgui/backends/imgui_impl_glfw.cpp"
    "${EXTERN_DIR}/imgui/backends/imgui_impl_opengl3.cpp"
)
target_include_directories(libera_timecode_imgui PUBLIC
    "${EXTERN_DIR}/imgui"
    "${EXTERN_DIR}/imgui/backends"
)
target_link_libraries(libera_timecode_imgui PUBLIC glfw)
if(APPLE)
    find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    target_link_libraries(libera_timecode_imgui PUBLIC ${OPENGL_FRAMEWORK})
elseif(WIN32)
    target_link_libraries(libera_timecode_imgui PUBLIC opengl32)
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(libera_timecode_imgui PUBLIC OpenGL::GL)
endif()

# nlohmann/json — header-only interface
add_library(libera_timecode_nlohmann_json INTERFACE)
target_include_directories(libera_timecode_nlohmann_json INTERFACE
    "${EXTERN_DIR}/nlohmann_json/include")

# RtAudio (uses its own CMakeLists)
set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
add_subdirectory("${EXTERN_DIR}/rtaudio" EXCLUDE_FROM_ALL)

# RtMidi — compile as static lib (avoid its CMake `uninstall` target collision
# with RtAudio's, plus we don't need its packaging logic)
add_library(libera_timecode_rtmidi STATIC
    "${EXTERN_DIR}/rtmidi/RtMidi.cpp"
)
target_include_directories(libera_timecode_rtmidi PUBLIC "${EXTERN_DIR}/rtmidi")
if(APPLE)
    target_compile_definitions(libera_timecode_rtmidi PRIVATE __MACOSX_CORE__)
    find_library(COREMIDI_FRAMEWORK CoreMIDI REQUIRED)
    find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(libera_timecode_rtmidi PUBLIC
        ${COREMIDI_FRAMEWORK} ${COREAUDIO_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
elseif(WIN32)
    target_compile_definitions(libera_timecode_rtmidi PRIVATE __WINDOWS_MM__)
    target_link_libraries(libera_timecode_rtmidi PUBLIC winmm)
else()
    find_package(ALSA)
    if(ALSA_FOUND)
        target_compile_definitions(libera_timecode_rtmidi PRIVATE __LINUX_ALSA__)
        target_include_directories(libera_timecode_rtmidi PRIVATE ${ALSA_INCLUDE_DIRS})
        target_link_libraries(libera_timecode_rtmidi PUBLIC ${ALSA_LIBRARIES} pthread)
    else()
        message(WARNING "ALSA not found — RtMidi will be a stub.")
    endif()
endif()
if(MSVC)
    target_compile_options(libera_timecode_rtmidi PRIVATE /W0)
else()
    target_compile_options(libera_timecode_rtmidi PRIVATE -w)
endif()

# libltc — small C library, no upstream CMake
add_library(libera_timecode_ltc STATIC
    "${EXTERN_DIR}/libltc/src/decoder.c"
    "${EXTERN_DIR}/libltc/src/encoder.c"
    "${EXTERN_DIR}/libltc/src/ltc.c"
    "${EXTERN_DIR}/libltc/src/timecode.c"
)
target_include_directories(libera_timecode_ltc PUBLIC "${EXTERN_DIR}/libltc/src")
if(MSVC)
    target_compile_options(libera_timecode_ltc PRIVATE /W0)
else()
    target_compile_options(libera_timecode_ltc PRIVATE -w)
endif()

# ---------------- App target ----------------

add_executable(libera_timecode
    src/gui_main.cpp
    src/LiberaApp.cpp
    src/Paths.cpp
    src/Settings.cpp
    src/Timecode.cpp
    src/TimecodeEngine.cpp
    src/outputs/UdpSender.cpp
    src/outputs/ArtnetOutput.cpp
    src/outputs/SntcOutput.cpp
    src/outputs/MidiOutput.cpp
    src/outputs/SmpteOutput.cpp
    src/fonts/RobotoMedium.cpp
    src/fonts/RobotoBold.cpp
    src/fonts/ForkAwesome.cpp
)

target_include_directories(libera_timecode PRIVATE src)
target_compile_definitions(libera_timecode PRIVATE
    APP_VERSION="${LIBERA_TIMECODE_APP_VERSION}")

target_link_libraries(libera_timecode PRIVATE
    libera_timecode_imgui
    libera_timecode_ltc
    libera_timecode_rtmidi
    libera_timecode_nlohmann_json
    rtaudio
)

if(APPLE)
    target_link_libraries(libera_timecode PRIVATE ${APPKIT_FRAMEWORK})
endif()

if(MSVC)
    target_compile_options(libera_timecode PRIVATE /W4 /permissive-)
else()
    target_compile_options(libera_timecode PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(APPLE)
    set_target_properties(libera_timecode PROPERTIES
        MACOSX_BUNDLE TRUE
        OUTPUT_NAME "${LIBERA_TIMECODE_APP_BUNDLE_NAME}"
        MACOSX_BUNDLE_BUNDLE_NAME "${LIBERA_TIMECODE_APP_BUNDLE_NAME}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.sebleedelisle.libera-timecode"
        MACOSX_BUNDLE_BUNDLE_VERSION "${LIBERA_TIMECODE_APP_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${LIBERA_TIMECODE_APP_VERSION}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
    )
endif()
