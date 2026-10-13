# Builds ONScripter Yuri as ONSRuntime.framework, an engine Mikage embeds in
# its iOS app. SDL itself is not built here: sdl2-compat is linked in
# statically and loads the SDL3.framework the app ships for all its engines.
#
#   cmake -S src/onsyuri_mikage -B build -G Xcode \
#     -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
#     -DCMAKE_OSX_ARCHITECTURES=arm64 \
#     -DMIKAGE_SDL3_FRAMEWORK=/path/to/ios-arm64/SDL3.framework
#   cmake --build build --config Release --target ONSRuntime

cmake_minimum_required(VERSION 3.24)
project(ONSRuntime LANGUAGES C CXX OBJC OBJCXX ASM)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(FATAL_ERROR "ONSRuntime is an iOS framework; configure with -DCMAKE_SYSTEM_NAME=iOS")
endif()

set(MIKAGE_SDL3_FRAMEWORK "" CACHE PATH "SDL3.framework the app embeds; sdl2-compat is built against its headers")
if(NOT EXISTS "${MIKAGE_SDL3_FRAMEWORK}/Headers/SDL.h")
    message(FATAL_ERROR "MIKAGE_SDL3_FRAMEWORK is not an SDL3.framework: '${MIKAGE_SDL3_FRAMEWORK}'")
endif()

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF)
set(ONSYURI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../onsyuri")

# SDL3's headers as <SDL3/...>, the way sdl2-compat includes them.
set(MIKAGE_SDL3_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/sdl3-include")
file(MAKE_DIRECTORY "${MIKAGE_SDL3_INCLUDE}")
file(CREATE_LINK "${MIKAGE_SDL3_FRAMEWORK}/Headers" "${MIKAGE_SDL3_INCLUDE}/SDL3" SYMBOLIC)
add_library(SDL3::Headers INTERFACE IMPORTED)
set_target_properties(SDL3::Headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${MIKAGE_SDL3_INCLUDE}")

# Dependencies, pinned. sdl2-compat comes from Mikage's fork, which adds iOS
# support (branch shared-sdl3/ons). The SDL satellite libraries use only
# their bundled codecs, so nothing else has to be built for iOS.
include(FetchContent)
FetchContent_Declare(sdl2_compat
    GIT_REPOSITORY https://github.com/itsCheney/sdl2-compat.git
    GIT_TAG 7f8f1c7293decab05c59aa28a2c9b095546efa29)
FetchContent_Declare(sdl2_image
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
    GIT_TAG 12cb2e40330d256d9b1329647be8f366546d715c # release-2.8.12
    GIT_SUBMODULES "")
FetchContent_Declare(sdl2_mixer
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_mixer.git
    GIT_TAG b208916aed9250fe434360e6c6a95f0697bb7b01 # release-2.8.2
    GIT_SUBMODULES "")
FetchContent_Declare(sdl2_ttf
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
    GIT_TAG 2a891473eaf05ba1707a4b7913e6c4db7de7458a # release-2.24.0
    GIT_SUBMODULES external/freetype)
FetchContent_Declare(lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG 1ab3208a1fceb12fca8f24ba57d6e13c5bff15e3 # v5.4.7
    # Lua has no CMake build; only fetch it.
    SOURCE_SUBDIR no-cmake)

set(SDL2COMPAT_STATIC ON)
set(SDL2COMPAT_TESTS OFF)
set(SDL2COMPAT_INSTALL OFF)
FetchContent_MakeAvailable(sdl2_compat)

set(SDL2IMAGE_INSTALL OFF)
set(SDL2IMAGE_SAMPLES OFF)
set(SDL2IMAGE_TESTS OFF)
set(SDL2IMAGE_VENDORED OFF)
set(SDL2IMAGE_DEPS_SHARED OFF)
set(SDL2IMAGE_BACKEND_IMAGEIO OFF)
set(SDL2IMAGE_BACKEND_STB ON)
set(SDL2IMAGE_AVIF OFF)
set(SDL2IMAGE_JXL OFF)
set(SDL2IMAGE_TIF OFF)
set(SDL2IMAGE_WEBP OFF)

set(SDL2MIXER_INSTALL OFF)
set(SDL2MIXER_SAMPLES OFF)
set(SDL2MIXER_VENDORED OFF)
set(SDL2MIXER_DEPS_SHARED OFF)
set(SDL2MIXER_CMD OFF)
set(SDL2MIXER_FLAC ON)
set(SDL2MIXER_FLAC_DRFLAC ON)
set(SDL2MIXER_FLAC_LIBFLAC OFF)
set(SDL2MIXER_GME OFF)
set(SDL2MIXER_MOD OFF)
set(SDL2MIXER_MP3 ON)
set(SDL2MIXER_MP3_MINIMP3 ON)
set(SDL2MIXER_MP3_MPG123 OFF)
set(SDL2MIXER_MIDI OFF)
set(SDL2MIXER_OPUS OFF)
set(SDL2MIXER_VORBIS STB)
set(SDL2MIXER_WAVE ON)
set(SDL2MIXER_WAVPACK OFF)

set(SDL2TTF_INSTALL OFF)
set(SDL2TTF_SAMPLES OFF)
set(SDL2TTF_VENDORED ON)
set(SDL2TTF_HARFBUZZ OFF)

FetchContent_MakeAvailable(sdl2_image sdl2_mixer sdl2_ttf lua)

# ONScripter includes the satellite headers as <SDL2/...>.
set(MIKAGE_SDL2_EXTRA_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/sdl2-extra-include")
file(MAKE_DIRECTORY "${MIKAGE_SDL2_EXTRA_INCLUDE}/SDL2")
file(CREATE_LINK "${sdl2_image_SOURCE_DIR}/include/SDL_image.h" "${MIKAGE_SDL2_EXTRA_INCLUDE}/SDL2/SDL_image.h" SYMBOLIC)
file(CREATE_LINK "${sdl2_mixer_SOURCE_DIR}/include/SDL_mixer.h" "${MIKAGE_SDL2_EXTRA_INCLUDE}/SDL2/SDL_mixer.h" SYMBOLIC)
file(CREATE_LINK "${sdl2_ttf_SOURCE_DIR}/SDL_ttf.h" "${MIKAGE_SDL2_EXTRA_INCLUDE}/SDL2/SDL_ttf.h" SYMBOLIC)

set(LUA_SOURCES
    lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldblib.c ldebug.c
    ldo.c ldump.c lfunc.c lgc.c linit.c liolib.c llex.c lmathlib.c lmem.c
    loadlib.c lobject.c lopcodes.c loslib.c lparser.c lstate.c lstring.c
    lstrlib.c ltable.c ltablib.c ltm.c lundump.c lutf8lib.c lvm.c lzio.c)
list(TRANSFORM LUA_SOURCES PREPEND "${lua_SOURCE_DIR}/")
add_library(mikage_lua STATIC ${LUA_SOURCES})
target_compile_definitions(mikage_lua PUBLIC LUA_USE_IOS)
target_include_directories(mikage_lua PUBLIC "${lua_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")

set(ONSYURI_SOURCES
    AnimationInfo.cpp
    coding2utf16.cpp
    DirectReader.cpp
    DirtyRect.cpp
    FontInfo.cpp
    gbk2utf16.cpp
    LUAHandler.cpp
    NsaReader.cpp
    ONScripter.cpp
    ONScripter_animation.cpp
    ONScripter_command.cpp
    ONScripter_effect.cpp
    ONScripter_effect_breakup.cpp
    ONScripter_event.cpp
    ONScripter_file.cpp
    ONScripter_file2.cpp
    ONScripter_image.cpp
    ONScripter_lut.cpp
    ONScripter_rmenu.cpp
    ONScripter_sound.cpp
    ONScripter_text.cpp
    Parallel.cpp
    resize_image.cpp
    SarReader.cpp
    ScriptHandler.cpp
    ScriptParser.cpp
    ScriptParser_command.cpp
    sjis2utf16.cpp
    builtin_dll/layer_oldmovie.cpp
    builtin_dll/layer_snow.cpp
    builtin_dll/ONScripter_effect_cascade.cpp
    builtin_dll/ONScripter_effect_trig.cpp)
list(TRANSFORM ONSYURI_SOURCES PREPEND "${ONSYURI_DIR}/")

# onscripter_main.cpp is replaced by the host API.
set(HOST_SOURCES
    MikageONSRuntime.mm
    MikageONSFiber.S)

add_library(ONSRuntime SHARED
    ${ONSYURI_SOURCES}
    ${HOST_SOURCES}
    MikageONSRuntime.h)

# The engine's waits, presents and exit() go through the host.
set_source_files_properties(${ONSYURI_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/MikageONSHooks.h")
set_source_files_properties(MikageONSRuntime.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

# The host includes ONScripter.h, so it must see the same configuration as
# the engine sources.
target_compile_definitions(ONSRuntime PRIVATE
    IOS
    MIKAGE_ONS_HOST
    USE_LUA
    USE_BUILTIN_LAYER_EFFECTS
    ENABLE_1BYTE_CHAR
    USE_PARALLEL)
target_compile_options(ONSRuntime PRIVATE
    $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wno-deprecated-declarations -Wno-invalid-source-encoding -Wno-writable-strings>)
target_include_directories(ONSRuntime PRIVATE
    "${ONSYURI_DIR}"
    "${MIKAGE_SDL2_EXTRA_INCLUDE}"
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ONSRuntime PRIVATE
    SDL2::SDL2-static
    SDL2_image
    SDL2_mixer
    SDL2_ttf
    mikage_lua
    bz2
    z
    "-framework Foundation"
    "-framework UIKit"
    "-framework AVFoundation"
    "-framework CoreText"
    "-framework CoreGraphics"
    "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/exported_symbols.txt")

set_target_properties(ONSRuntime PROPERTIES
    FRAMEWORK TRUE
    PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/MikageONSRuntime.h"
    MACOSX_FRAMEWORK_IDENTIFIER "moe.cheney233.mikage.ons-runtime"
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
    XCODE_ATTRIBUTE_DEFINES_MODULE "YES"
    XCODE_ATTRIBUTE_CLANG_ENABLE_MODULES "YES"
    XCODE_ATTRIBUTE_MODULEMAP_FILE "${CMAKE_CURRENT_SOURCE_DIR}/module.modulemap"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_LD_DYLIB_INSTALL_NAME "@rpath/ONSRuntime.framework/ONSRuntime"
    XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "@executable_path/Frameworks @loader_path/..")
