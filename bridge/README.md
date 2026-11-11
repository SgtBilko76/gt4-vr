# PCSX2 GS bridge (renderer of the Quest APKs)

The APKs draw the game with `librrv-pcsx2-gs-bridge.so`: PCSX2's GS renderer and SPU2 core, built
as a library for Android arm64. This folder is its complete source. It is licensed under the
**GNU General Public License v3.0 or later** ([LICENSE](LICENSE)), unlike the rest of this repository
(MIT).

| Folder | What it is |
|---|---|
| `tools/pcsx2-gs-bridge/` | The bridge library and its C API (`rrv_pcsx2_gs_bridge.h`) |
| `tools/pcsx2-spu2/` | PCSX2's SPU2 sound core as a library |
| `quest/bridge/` | Android build (CMake, NDK script, stereo/VR helpers) |
| `quest/bridge/patches/` | 44 patches on top of the PCSX2 fork, base commit in `patches/BASE` |

## Where it comes from
- The bridge is part of [Fukami](https://github.com/danisandoval/fukami) by Dani Sandoval (GPL-3.0),
  with changes for the Quest ports. These folders replace the ones of the same name in Fukami;
  Fukami's `src/audio` and `cmake` are used unchanged (upstream commit `2ecb281`).
- PCSX2 comes from [ARMSX2-3D](https://github.com/SgtBilko76/ARMSX2-3D), an ARM build of
  [PCSX2](https://pcsx2.net) (GPL-3.0), at commit `04634f1b`, plus the patches in `quest/bridge/patches/`.

## Building
1. Clone Fukami and check out commit `2ecb281`.
2. Copy `tools/pcsx2-gs-bridge`, `tools/pcsx2-spu2` and `quest/bridge` from this folder over the
   same paths in the Fukami checkout.
3. Run `quest/bridge/build_android_bridge.sh` there. It clones ARMSX2-3D at the commit in
   `quest/bridge/patches/BASE`, applies the patches with `git am` and builds the library with the
   Android NDK (see the script's header for the options).
4. Point `BRIDGE_BUILD` at the build folder when running `quest/tools/build_quest.sh`.
