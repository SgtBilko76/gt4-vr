# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fukami's PCSX2 GS bridge (librrv-pcsx2-gs-bridge.so) and the PCSX2 SPU2 core
# (librrv_pcsx2_spu2.so + .a) for Android arm64-v8a / Meta Quest.
#
# The PCSX2 source is ARMSX2 (the ARM64/Android PCSX2 fork) with Fukami's bridge
# patch series merged in: branch `fukami-bridge` of build-deps/armsx2-pcsx2, which
# quest/bridge/patches/ reproduces from ARMSX2-3D. Its common/ and pcsx2/ are built
# exactly as ARMSX2's APK entry (platforms/android/app/src/main/cpp/CMakeLists.txt)
# builds them -- same Android cmake modules, same vendored 3rdparty -- minus the APK's
# own targets (emucore JNI, savers, librashader, Discord, OpenXR layer).
#
# Configure with the NDK toolchain file (see quest/bridge/build_android_bridge.sh):
#   cmake -S quest/bridge -B build-android-bridge -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
#     -DCMAKE_BUILD_TYPE=Release -DRRV_ARMSX2_SOURCE_DIR=<armsx2-pcsx2 checkout>
cmake_minimum_required(VERSION 3.21...3.25)

cmake_policy(SET CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

project(RrvQuestPcsx2Bridge C CXX)

if(NOT ANDROID OR NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "quest/bridge is the Android arm64-v8a build; configure it with the NDK toolchain file "
                        "and -DANDROID_ABI=arm64-v8a")
endif()

get_filename_component(RRV_FUKAMI_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(RRV_ARMSX2_SOURCE_DIR "${RRV_FUKAMI_ROOT}/build-deps/armsx2-pcsx2" CACHE PATH
    "ARMSX2 checkout with the Fukami bridge patches (branch fukami-bridge)")
set(RRV_PCSX2_SPU2_SOURCE_DIR "${RRV_FUKAMI_ROOT}/build-deps/pcsx2-2.8.2" CACHE PATH
    "Pinned PCSX2 2.8.2 checkout the SPU2 core is compiled from (unmodified, as on desktop)")
option(RRV_QUEST_BUILD_SPU2 "Build the PCSX2 SPU2 core library" ON)
option(RRV_QUEST_BUILD_GS_BRIDGE "Build the PCSX2 GS bridge" ON)

set(RRV_GS_BRIDGE_TOOLS_DIR "${RRV_FUKAMI_ROOT}/tools/pcsx2-gs-bridge")

# --------------------------------------------------------------------------------------
# SPU2 core. Built first and before any PCSX2 global compile settings exist, so it gets
# exactly the flags tools/pcsx2-spu2 gives it on desktop (the product adds it "before the
# project-wide USE_SSE2NEON definition", cmake/RrvProductOwnedIncludes.cmake).
# --------------------------------------------------------------------------------------
if(RRV_QUEST_BUILD_SPU2)
    set(RRV_PCSX2_SOURCE_DIR "${RRV_PCSX2_SPU2_SOURCE_DIR}" CACHE PATH "" FORCE)
    set(RRV_SPU2_BUILD_TESTS ON CACHE BOOL "" FORCE)  # spu2_selftest: runnable on the headset over adb
    set(RRV_SPU2_TSAN_TESTS OFF CACHE BOOL "" FORCE)
    set(RRV_RSPU2_DRIVER_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${RRV_FUKAMI_ROOT}/tools/pcsx2-spu2" pcsx2-spu2)
    # The desktop product links the static archive. For an APK whose runtime is itself a
    # shared library the same archive can be linked in, or this shared library loaded:
    # it exports exactly the rrv_spu2_* C ABI of tools/pcsx2-spu2/rrv_pcsx2_spu2.h.
    set_target_properties(rrv_pcsx2_spu2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(rrv_pcsx2_spu2_shared SHARED "${CMAKE_CURRENT_LIST_DIR}/spu2_shared_exports.cpp")
    target_link_libraries(rrv_pcsx2_spu2_shared PRIVATE rrv_pcsx2_spu2)
    target_link_options(rrv_pcsx2_spu2_shared PRIVATE
        "LINKER:--version-script=${CMAKE_CURRENT_LIST_DIR}/spu2.version"
        "LINKER:-z,max-page-size=16384")
    set_target_properties(rrv_pcsx2_spu2_shared PROPERTIES
        OUTPUT_NAME rrv_pcsx2_spu2
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pcsx2-spu2"
        LINK_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/spu2.version")
endif()

if(NOT RRV_QUEST_BUILD_GS_BRIDGE)
    return()
endif()

# --------------------------------------------------------------------------------------
# ARMSX2 core, as its Android entry CMakeLists sets it up.
# --------------------------------------------------------------------------------------
set(ARMSX2_ROOT "${RRV_ARMSX2_SOURCE_DIR}")
set(ARMSX2_ANDROID_CPP_DIR "${ARMSX2_ROOT}/platforms/android/app/src/main/cpp")
foreach(_rrv_required IN ITEMS common/CMakeLists.txt pcsx2/CMakeLists.txt pcsx2/GS/GS.cpp
                               pcsx2-gsrunner/Main.cpp platforms/android/app/src/main/cpp/cmake/SearchForStuff.cmake)
    if(NOT EXISTS "${ARMSX2_ROOT}/${_rrv_required}")
        message(FATAL_ERROR "RRV_ARMSX2_SOURCE_DIR is not an ARMSX2 checkout (missing ${_rrv_required}): ${ARMSX2_ROOT}")
    endif()
endforeach()
file(STRINGS "${ARMSX2_ROOT}/pcsx2/GS/GS.h" _rrv_bridge_entry REGEX "GSSnapshotLocalMemory")
if(NOT _rrv_bridge_entry)
    message(FATAL_ERROR "${ARMSX2_ROOT} lacks the Fukami bridge patches (GSSnapshotLocalMemory); "
                        "check out branch fukami-bridge or apply quest/bridge/patches")
endif()

list(PREPEND CMAKE_MODULE_PATH "${ARMSX2_ANDROID_CPP_DIR}/cmake")

set(ENABLE_QT_UI OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBRETRO OFF CACHE BOOL "" FORCE)
# Custom (Turnip) Vulkan drivers through libadrenotools are an APK feature: Quest runs Meta's
# own Adreno driver and the bridge loads the system libvulkan.so.
set(ARMSX2_USE_ADRENOTOOLS OFF CACHE BOOL "" FORCE)
# No PCSX2 audio output in the bridge: Fukami mixes through tools/pcsx2-spu2.
set(ARMSX2_USE_OBOE OFF CACHE BOOL "" FORCE)
set(ARMSX2_USE_BUNDLED_SHADERC ON CACHE BOOL "" FORCE)
set(ARMSX2_ENABLE_LSFG OFF CACHE BOOL "" FORCE)
set(ARMSX2_EMBED_RESOURCES OFF CACHE BOOL "" FORCE)
set(TOP_CMAKE_WAS_SOURCED TRUE)

include(Pcsx2Utils)
check_no_parenthesis_in_path()
detect_operating_system()
detect_compiler()
include(BuildParameters)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/ArmsX2AndroidDeps.cmake")

get_git_version_info()
write_svnrev_h()

add_subdirectory("${ARMSX2_ROOT}/common" "${CMAKE_BINARY_DIR}/common")
add_subdirectory("${ARMSX2_ROOT}/3rdparty/libretro" "${CMAKE_BINARY_DIR}/3rdparty/libretro")
add_subdirectory("${ARMSX2_ROOT}/pcsx2" "${CMAKE_BINARY_DIR}/pcsx2")

# The bridge patch's RRV_PCSX2_GS_BRIDGE_SOURCE_DIR hook lives in ARMSX2's desktop top-level
# CMakeLists, which this build does not use; the same switch, here.
option(RRV_PCSX2_GS_BRIDGE_SW_JIT
       "Diagnostic: use PCSX2's runtime SW rasterizer JIT instead of the static functions" OFF)
if(RRV_PCSX2_GS_BRIDGE_SW_JIT)
    target_compile_definitions(PCSX2 PUBLIC RRV_PCSX2_GS_SW_JIT=1)
endif()

# --------------------------------------------------------------------------------------
# The bridge: Fukami's bridge.cpp + gsrunner host shim, with the Android surface adapter
# in place of linux_surface_adapter.cpp. gsrunner_host.cpp #includes ARMSX2's
# pcsx2-gsrunner/Main.cpp, whose helpers live in three more sources of that directory.
# --------------------------------------------------------------------------------------
add_library(rrv-pcsx2-gs-bridge SHARED
    "${RRV_GS_BRIDGE_TOOLS_DIR}/bridge.cpp"
    "${RRV_GS_BRIDGE_TOOLS_DIR}/gsrunner_host.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/android_surface_adapter.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/android_stereo.cpp"
    "${ARMSX2_ROOT}/pcsx2-gsrunner/GSLadder.cpp"
    "${ARMSX2_ROOT}/pcsx2-gsrunner/GSReplayPayload.cpp"
    "${ARMSX2_ROOT}/pcsx2-gsrunner/RenderDocCapture.cpp"
)
option(RRV_PCSX2_GS_CANONICAL_RECEIPTS
    "Compile bounded M2 consumer-side receipt support into the diagnostic bridge" OFF)
if(RRV_PCSX2_GS_CANONICAL_RECEIPTS)
    target_sources(rrv-pcsx2-gs-bridge PRIVATE "${RRV_GS_BRIDGE_TOOLS_DIR}/canonical_receipt.cpp")
    target_compile_definitions(rrv-pcsx2-gs-bridge PRIVATE RRV_PCSX2_GS_CANONICAL_RECEIPTS=1)
endif()
target_include_directories(rrv-pcsx2-gs-bridge
    PUBLIC "${RRV_GS_BRIDGE_TOOLS_DIR}"
    PRIVATE
        "${CMAKE_BINARY_DIR}/common/include"
        "${ARMSX2_ROOT}"
        "${ARMSX2_ROOT}/pcsx2"
        "${ARMSX2_ROOT}/pcsx2-gsrunner"
)
target_compile_definitions(rrv-pcsx2-gs-bridge PRIVATE RRV_PCSX2_GS_BRIDGE_BUILD=1)
# shaderc is called directly on Android (VKShaderCache.cpp); android/log are left to the final
# link by the core, as for ARMSX2's emucore and its NDK gsrunner.
target_link_libraries(rrv-pcsx2-gs-bridge PRIVATE PCSX2_FLAGS PCSX2 shaderc android log)
target_link_options(rrv-pcsx2-gs-bridge PRIVATE
    # Export the bridge ABI and nothing else (the core and its static 3rdparty stay internal).
    "LINKER:--version-script=${CMAKE_CURRENT_LIST_DIR}/gs_bridge.version"
    "LINKER:--gc-sections")
set_target_properties(rrv-pcsx2-gs-bridge PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    LINK_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/gs_bridge.version"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pcsx2-gs-bridge")

# Runtime resources (shaders, fonts, GameIndex.yaml...). On Android they are not beside the
# library (that is the read-only APK lib dir): the app copies this directory out of its APK
# assets and names it with rrv_pcsx2_gs_bridge_android_set_paths() or
# RRV_PCSX2_GS_RESOURCE_DIR (quest/bridge/README.md). Staged here for packaging.
add_custom_command(TARGET rrv-pcsx2-gs-bridge POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${ARMSX2_ROOT}/bin/resources"
            "$<TARGET_FILE_DIR:rrv-pcsx2-gs-bridge>/resources"
    VERBATIM)
