# SPDX-License-Identifier: GPL-3.0-or-later
#
# Source ARMSX2's vendored Android 3rdparty the way ARMSX2's own APK build does,
# from a project whose top-level source directory is NOT ARMSX2's Android cpp dir.
#
# ARMSX2's SearchForStuff.cmake (platforms/android/app/src/main/cpp/cmake) is written
# for its thin Android entry CMakeLists, where CMAKE_SOURCE_DIR == the cpp dir: it says
# `add_subdirectory(3rdparty/<lib> ...)` relative to the including directory and
# `${CMAKE_SOURCE_DIR}/3rdparty/...` for shaderc and libjpeg-turbo. Rather than fork the
# module, read it at configure time, point both spellings at the cpp dir (with an
# explicit binary dir, which an out-of-tree add_subdirectory requires) and include the
# rewritten copy. Any change ARMSX2 makes to its dependency sourcing flows through.
#
# In: ARMSX2_ANDROID_CPP_DIR (absolute path of platforms/android/app/src/main/cpp).

if(NOT IS_DIRECTORY "${ARMSX2_ANDROID_CPP_DIR}/3rdparty")
    message(FATAL_ERROR "ARMSX2 Android cpp dir has no 3rdparty: ${ARMSX2_ANDROID_CPP_DIR}")
endif()

set(_rrv_sfs_in "${ARMSX2_ANDROID_CPP_DIR}/cmake/SearchForStuff.cmake")
file(READ "${_rrv_sfs_in}" _rrv_sfs)

# ${CMAKE_SOURCE_DIR}/3rdparty -> the cpp dir's 3rdparty (literal text; expands when included).
string(REPLACE "\${CMAKE_SOURCE_DIR}/3rdparty" "\${ARMSX2_ANDROID_CPP_DIR}/3rdparty" _rrv_sfs "${_rrv_sfs}")
# add_subdirectory(3rdparty/<path> ...) -> absolute source + binary dir.
string(REGEX REPLACE
    "add_subdirectory\\(([ \t]*)3rdparty/([A-Za-z0-9_./+-]+)"
    "add_subdirectory(\\1\"\${ARMSX2_ANDROID_CPP_DIR}/3rdparty/\\2\" \"\${CMAKE_BINARY_DIR}/3rdparty/\\2\""
    _rrv_sfs "${_rrv_sfs}")

string(FIND "${_rrv_sfs}" "add_subdirectory(3rdparty/" _rrv_left)
if(NOT _rrv_left EQUAL -1)
    message(FATAL_ERROR "ArmsX2AndroidDeps: an add_subdirectory(3rdparty/...) in ${_rrv_sfs_in} was not rewritten")
endif()

set(_rrv_sfs_out "${CMAKE_BINARY_DIR}/rrv-generated/SearchForStuff.android.cmake")
file(WRITE "${_rrv_sfs_out}.tmp" "${_rrv_sfs}")
file(COPY_FILE "${_rrv_sfs_out}.tmp" "${_rrv_sfs_out}" ONLY_IF_DIFFERENT)
include("${_rrv_sfs_out}")
