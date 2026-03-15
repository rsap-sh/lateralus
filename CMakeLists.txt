cmake_minimum_required(VERSION 3.16)
project(voicechat C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Git version (commit count + short hash) ───────────────────────────────
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-list --count HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE VC_COMMIT_COUNT
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE VC_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT VC_COMMIT_COUNT)
    set(VC_COMMIT_COUNT "0")
endif()
if(NOT VC_COMMIT_HASH)
    set(VC_COMMIT_HASH "unknown")
endif()
message(STATUS "Build version: #${VC_COMMIT_COUNT} (${VC_COMMIT_HASH})")

# macOS deployment target — can be overridden on the cmake command line:
#   ARM  build: -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0  (M1 minimum)
#   Intel build: -DCMAKE_OSX_DEPLOYMENT_TARGET=10.13 (High Sierra, 2009+ Macs)
# Default to 12.0 so local ARM builds don't accidentally target too low.
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
endif()

# ── Compiler flags ────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/O2 /W3)
else()
    add_compile_options(-O2 -Wall -Wextra -Wpedantic)
endif()

# ── Server (Linux only) ───────────────────────────────────────────────────
if(UNIX AND NOT APPLE)
    add_executable(voicechat-server
        server/server.c
    )
    target_include_directories(voicechat-server PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(voicechat-server pthread m)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(voicechat-server PRIVATE _GNU_SOURCE)
    endif()
endif()

# ── Client ────────────────────────────────────────────────────────────────
#
# We bypass pkg-config entirely and use find_path/find_library directly.
# Reason: pkg-config on macOS Homebrew returns Cellar paths like
#   /opt/homebrew/Cellar/opus/1.6.1/include/opus
# which is one level too deep — #include <opus/opus.h> then resolves to
#   .../include/opus/opus/opus.h  (double opus/ dir)
# find_path searching for "opus/opus.h" naturally finds the correct parent.

# Homebrew prefix detection (Apple Silicon: /opt/homebrew, Intel: /usr/local)
if(APPLE)
    execute_process(
        COMMAND brew --prefix
        OUTPUT_VARIABLE HOMEBREW_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

set(_INCLUDE_HINTS
    /usr/include
    /usr/local/include
    /opt/homebrew/include
    "${HOMEBREW_PREFIX}/include"
)
set(_LIB_HINTS
    /usr/lib
    /usr/local/lib
    /opt/homebrew/lib
    "${HOMEBREW_PREFIX}/lib"
)

find_path(OPUS_INCLUDE_DIR
    NAMES opus/opus.h
    HINTS ${_INCLUDE_HINTS}
)

# STATIC_LIBS=ON: prefer .a over .so/.dylib.
# Enabled automatically on Apple (avoids Team ID dylib issues on macOS 15+).
# Pass -DSTATIC_LIBS=ON on the cmake command line for a fully static Linux build
# (e.g. the musl/Alpine CI job that produces a zero-dependency portable binary).
option(STATIC_LIBS "Prefer static (.a) libraries for Opus and PortAudio" OFF)

# CMAKE_FIND_LIBRARY_SUFFIXES controls the order find_library tries extensions.
# Default on Linux is .so first. Flip to .a first when a static build is wanted.
# Note: NAMES libopus.a opus is NOT the right idiom — CMake applies lib-prefix
# and suffix transformations to each name, so "libopus.a" becomes "liblibopus.a.so"
# which never matches. Use NAMES opus with suffix order to drive .a preference.
if(APPLE OR STATIC_LIBS)
    set(CMAKE_FIND_LIBRARY_SUFFIXES .a .so .dylib)
endif()

find_library(OPUS_LIBRARY
    NAMES opus
    HINTS ${_LIB_HINTS}
)
find_library(PORTAUDIO_LIBRARY
    NAMES portaudio
    HINTS ${_LIB_HINTS}
)

find_path(PORTAUDIO_INCLUDE_DIR
    NAMES portaudio.h
    HINTS ${_INCLUDE_HINTS}
)

# ── libvpx (VP9 software encoder/decoder — screen sharing) ─────────────
find_path(VPX_INCLUDE_DIR
    NAMES vpx/vpx_encoder.h
    HINTS ${_INCLUDE_HINTS}
)
find_library(VPX_LIBRARY
    NAMES vpx
    HINTS ${_LIB_HINTS}
)

message(STATUS "Opus include:     ${OPUS_INCLUDE_DIR}")
message(STATUS "Opus lib:         ${OPUS_LIBRARY}")
message(STATUS "PortAudio include:${PORTAUDIO_INCLUDE_DIR}")
message(STATUS "PortAudio lib:    ${PORTAUDIO_LIBRARY}")
message(STATUS "VPX include:      ${VPX_INCLUDE_DIR}")
message(STATUS "VPX lib:          ${VPX_LIBRARY}")

# Screen sharing sources (compiled into both CLI and GUI clients)
set(_SCREEN_SHARE_SRCS
    client/screen_capture.c
    client/encoder.c
    client/screen_share.c
)

if(OPUS_INCLUDE_DIR AND OPUS_LIBRARY AND PORTAUDIO_INCLUDE_DIR AND PORTAUDIO_LIBRARY
   AND VPX_INCLUDE_DIR AND VPX_LIBRARY)
    add_executable(voicechat-client
        client/client.c
        ${_SCREEN_SHARE_SRCS}
    )
    target_include_directories(voicechat-client PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${OPUS_INCLUDE_DIR}
        ${PORTAUDIO_INCLUDE_DIR}
        ${VPX_INCLUDE_DIR}
    )
    target_link_libraries(voicechat-client
        ${OPUS_LIBRARY}
        ${PORTAUDIO_LIBRARY}
        ${VPX_LIBRARY}
        m
    )
    if(WIN32)
        target_link_libraries(voicechat-client ws2_32 winhttp shlwapi d3d11 dxgi mfplat mfuuid mf ole32)
    else()
        target_link_libraries(voicechat-client pthread)
        # Static portaudio on Linux pulls in ALSA — link it explicitly.
        # find_library prefers .a when STATIC_LIBS=ON so this also goes static.
        if(UNIX AND NOT APPLE AND STATIC_LIBS)
            find_library(ALSA_LIBRARY
                NAMES libasound.a asound
                HINTS ${_LIB_HINTS} /usr/lib /usr/lib/x86_64-linux-musl
            )
            if(ALSA_LIBRARY)
                target_link_libraries(voicechat-client ${ALSA_LIBRARY})
            else()
                target_link_libraries(voicechat-client asound)
            endif()
        endif()
    endif()
    if(APPLE)
        target_link_libraries(voicechat-client
            "-framework CoreAudio"
            "-framework AudioToolbox"
            "-framework AudioUnit"
            "-framework CoreServices"
            "-framework CoreFoundation"
        )
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(voicechat-client PRIVATE _GNU_SOURCE)
    endif()
    message(STATUS "Client will be built")
else()
    # Only error if no server target exists (i.e. not a server-only build).
    # Server-only builds (e.g. Alpine/musl) don't need client dependencies.
    if(NOT TARGET voicechat-server)
        if(NOT OPUS_INCLUDE_DIR OR NOT OPUS_LIBRARY)
            message(FATAL_ERROR "libopus not found. Install: brew install opus | apt install libopus-dev | pacman -S mingw-w64-x86_64-opus")
        endif()
        if(NOT PORTAUDIO_INCLUDE_DIR OR NOT PORTAUDIO_LIBRARY)
            message(FATAL_ERROR "portaudio not found. Install: brew install portaudio | apt install portaudio19-dev | pacman -S mingw-w64-x86_64-portaudio")
        endif()
        if(NOT VPX_INCLUDE_DIR OR NOT VPX_LIBRARY)
            message(FATAL_ERROR "libvpx not found. Install: brew install libvpx | apt install libvpx-dev | pacman -S mingw-w64-x86_64-libvpx")
        endif()
    else()
        message(STATUS "Client libraries not found — only server will be built")
    endif()
endif()

# ── GUI client (voicechat) ────────────────────────────────────────────────
#
# Platform dispatch:
#   macOS   → native AppKit (Cocoa)  — gui_macos.m
#              Info.plist embedded at link time for CFBundleName + Retina
#   Linux   → native GTK3            — gui_gtk.c
#   Windows → SDL2 + Dear ImGui      — gui.cpp
#
# All variants call the same engine API (vc_api.h / client.c).

if(APPLE)
    # ── macOS: native Cocoa/AppKit GUI ────────────────────────────────
    add_executable(voicechat
        client/client.c
        client/gui_macos.m
        ${_SCREEN_SHARE_SRCS}
    )
    target_include_directories(voicechat PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${OPUS_INCLUDE_DIR}
        ${PORTAUDIO_INCLUDE_DIR}
        ${VPX_INCLUDE_DIR}
    )
    target_link_libraries(voicechat
        ${OPUS_LIBRARY}
        ${PORTAUDIO_LIBRARY}
        ${VPX_LIBRARY}
        m pthread
        "-framework Cocoa"
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework AudioUnit"
        "-framework CoreServices"
        "-framework CoreFoundation"
        "-framework CoreGraphics"
        "-framework VideoToolbox"
        "-framework CoreMedia"
        "-framework CoreVideo"
    )
    # Embed Info.plist so the binary gets CFBundleName=Lateralus and
    # NSHighResolutionCapable=true without needing a .app bundle.
    target_link_options(voicechat PRIVATE
        "-Wl,-sectcreate,__TEXT,__info_plist,${CMAKE_SOURCE_DIR}/client/Info.plist"
    )
    target_compile_definitions(voicechat PRIVATE VC_GUI_BUILD
        VC_COMMIT_COUNT="${VC_COMMIT_COUNT}" VC_COMMIT_HASH="${VC_COMMIT_HASH}")
    message(STATUS "GUI client (voicechat) will be built — native AppKit (macOS)")

elseif(UNIX AND NOT APPLE)
    # ── Linux: native GTK3 GUI ────────────────────────────────────────
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GTK3 QUIET gtk+-3.0)
    endif()

    if(GTK3_FOUND)
        add_executable(voicechat
            client/client.c
            client/gui_gtk.c
            ${_SCREEN_SHARE_SRCS}
        )
        target_include_directories(voicechat PRIVATE
            ${CMAKE_SOURCE_DIR}
            ${OPUS_INCLUDE_DIR}
            ${PORTAUDIO_INCLUDE_DIR}
            ${VPX_INCLUDE_DIR}
            ${GTK3_INCLUDE_DIRS}
        )
        target_compile_options(voicechat PRIVATE ${GTK3_CFLAGS_OTHER})
        target_link_libraries(voicechat
            ${OPUS_LIBRARY}
            ${PORTAUDIO_LIBRARY}
            ${VPX_LIBRARY}
            ${GTK3_LIBRARIES}
            m pthread X11 Xext
        )
        target_compile_definitions(voicechat PRIVATE VC_GUI_BUILD _GNU_SOURCE
            VC_COMMIT_COUNT="${VC_COMMIT_COUNT}" VC_COMMIT_HASH="${VC_COMMIT_HASH}")
        message(STATUS "GUI client (voicechat) will be built — native GTK3 (Linux)")
    else()
        message(STATUS "GTK3 not found — GUI client will NOT be built on Linux.\n"
                       "  Install: apt install libgtk-3-dev")
    endif()

else()
    # ── Windows: SDL2 + Dear ImGui GUI ───────────────────────────────────
    find_package(SDL2 REQUIRED)
    include(FetchContent)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.6
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(imgui)
    if(NOT imgui_POPULATED)
        FetchContent_Populate(imgui)
    endif()

    set(_IMGUI_DIR  ${imgui_SOURCE_DIR})
    set(_IMGUI_BACK ${imgui_SOURCE_DIR}/backends)

    set(_IMGUI_SRCS
        ${_IMGUI_DIR}/imgui.cpp
        ${_IMGUI_DIR}/imgui_draw.cpp
        ${_IMGUI_DIR}/imgui_tables.cpp
        ${_IMGUI_DIR}/imgui_widgets.cpp
        ${_IMGUI_BACK}/imgui_impl_sdl2.cpp
        ${_IMGUI_BACK}/imgui_impl_sdlrenderer2.cpp
    )

    # Suppress warnings from ImGui sources (they're upstream, not ours)
    set_source_files_properties(${_IMGUI_SRCS} PROPERTIES COMPILE_FLAGS "-w")

    # ── voicechat target ────────────────────────────────────────────────
    add_executable(voicechat
        client/client.c       # engine, compiled as C11
        client/gui.cpp        # UI,     compiled as C++17
        ${_SCREEN_SHARE_SRCS}
        ${_IMGUI_SRCS}
    )

    target_include_directories(voicechat PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${OPUS_INCLUDE_DIR}
        ${PORTAUDIO_INCLUDE_DIR}
        ${VPX_INCLUDE_DIR}
        ${_IMGUI_DIR}
        ${_IMGUI_BACK}
    )

    # SDL2 — handle imported target (non-Apple) and manual find_library (Apple)
    if(TARGET SDL2::SDL2)
        if(TARGET SDL2::SDL2main)
            target_link_libraries(voicechat SDL2::SDL2main)
        endif()
        target_link_libraries(voicechat SDL2::SDL2)
    else()
        target_include_directories(voicechat PRIVATE
            ${SDL2_INCLUDE_DIR} ${SDL2_INCLUDE_DIRS})
        target_link_libraries(voicechat ${SDL2_LIBRARIES})
    endif()

    target_link_libraries(voicechat
        ${OPUS_LIBRARY}
        ${PORTAUDIO_LIBRARY}
        ${VPX_LIBRARY}
        m
        ws2_32 winhttp shlwapi
        d3d11 dxgi
        mfplat mfuuid mf ole32
    )
    target_compile_definitions(voicechat PRIVATE VC_GUI_BUILD
        VC_COMMIT_COUNT="${VC_COMMIT_COUNT}" VC_COMMIT_HASH="${VC_COMMIT_HASH}")
    # Hide the console window — GUI app, logs are toggled via the in-app button
    set_target_properties(voicechat PROPERTIES WIN32_EXECUTABLE TRUE)

    message(STATUS "GUI client (voicechat) will be built — SDL2/ImGui (Windows)")
    message(STATUS "ImGui source: ${_IMGUI_DIR}")

endif() # if(APPLE) / elseif(UNIX) / else(Windows)

# ── Install ───────────────────────────────────────────────────────────────
if(TARGET voicechat-server)
    install(TARGETS voicechat-server DESTINATION bin)
endif()
if(TARGET voicechat-client)
    install(TARGETS voicechat-client DESTINATION bin)
endif()
if(TARGET voicechat)
    install(TARGETS voicechat DESTINATION bin)
endif()
