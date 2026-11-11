#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fetch the shaderc third-party sources ARMSX2's vendored shaderc needs to build
# (SPIRV-Headers, SPIRV-Tools, glslang) at the exact revisions pinned in its DEPS file.
# ARMSX2's own CI does the same with utils/git-sync-deps (they are neither vendored nor
# git submodules); this script only takes the three the library build needs (no
# googletest/abseil/re2/effcee: shaderc tests are off) and fetches them shallowly.
#
#   quest/bridge/fetch_shaderc_deps.sh [<armsx2-pcsx2 checkout>]
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/../.." && pwd)
armsx2=${1:-${ARMSX2_SOURCE:-"$root/build-deps/armsx2-pcsx2"}}
shaderc="$armsx2/platforms/android/app/src/main/cpp/3rdparty/shaderc"
deps="$shaderc/DEPS"
[[ -f "$deps" ]] || { echo "error: no shaderc DEPS file at $deps" >&2; exit 1; }

rev() { sed -n "s/^ *'$1_revision': *'\([0-9a-f]*\)'.*/\1/p" "$deps"; }

fetch() {
    # fetch <url> <third_party subdir> <revision>
    local url=$1 dir="$shaderc/third_party/$2" want=$3
    if [[ -d "$dir/.git" ]] && [[ "$(git -C "$dir" rev-parse HEAD)" == "$want" ]]; then
        echo "== $2 already at $want"
        return
    fi
    echo "== $2 @ $want"
    rm -rf "$dir"
    git init -q "$dir"
    git -C "$dir" remote add origin "$url"
    git -C "$dir" fetch -q --depth 1 origin "$want"
    git -C "$dir" checkout -q FETCH_HEAD
}

fetch https://github.com/KhronosGroup/SPIRV-Headers.git spirv-headers "$(rev spirv_headers)"
fetch https://github.com/KhronosGroup/SPIRV-Tools.git spirv-tools "$(rev spirv_tools)"
fetch https://github.com/KhronosGroup/glslang.git glslang "$(rev glslang)"
