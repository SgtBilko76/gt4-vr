// SPDX-License-Identifier: GPL-3.0-or-later
//
// Quest VR stereo controls of the Android bridge. The GS side lives in the ARMSX2 PCSX2 fork the
// bridge is built from (GSStereo in pcsx2/GS/Renderers/Common/GSRenderer.h): with stereo on, the
// presentation surface is double-width, each half is one eye, and the frame is reprojected per
// eye by the depth buffer of its main 3D pass. The Quest host (fukami-wt-runtime quest/host)
// finds these with dlsym(RTLD_NOLOAD) like rrv_pcsx2_gs_bridge_set_overlay; they only store
// atomics, so any thread may call them at any time and the next present picks them up.

#include "GS/Renderers/Common/GSRenderer.h"

#include <atomic>

// tools/pcsx2-gs-bridge/bridge.cpp: the Quest VR race HUD box.
namespace rrv_quest_hud
{
extern std::atomic<float> vr_scale;
extern std::atomic<float> vr_x;
extern std::atomic<int> present_aspect;
extern std::atomic<int> vsync_presented;
} // namespace rrv_quest_hud

#define RRV_QUEST_EXPORT extern "C" __attribute__((visibility("default")))

RRV_QUEST_EXPORT void rrv_pcsx2_gs_bridge_android_set_stereo(int enabled, float separation, float convergence)
{
    GSStereo::separation.store(separation, std::memory_order_relaxed);
    GSStereo::convergence.store(convergence, std::memory_order_relaxed);
    GSStereo::enabled.store(enabled != 0, std::memory_order_relaxed);
}

RRV_QUEST_EXPORT void rrv_pcsx2_gs_bridge_android_set_stereo_options(int reproject, float debug_depth, int flip_y)
{
    GSStereo::reproject.store(reproject != 0, std::memory_order_relaxed);
    GSStereo::debug_depth.store(debug_depth, std::memory_order_relaxed);
    GSStereo::flip_y.store(flip_y != 0, std::memory_order_relaxed);
}

// Immersive race view: bring the race HUD into the middle of the frame. scale = size and distance
// from the centre of every HUD element relative to the frame (0 = off: the desktop rules), x = an
// extra width factor (4:3 proportions at the frame's aspect).
RRV_QUEST_EXPORT void rrv_pcsx2_gs_bridge_android_set_hud_vr(float scale, float x)
{
    rrv_quest_hud::vr_x.store(x, std::memory_order_relaxed);
    rrv_quest_hud::vr_scale.store(scale, std::memory_order_relaxed);
}

// Depth packing scale of the reprojection (GSStereo::depth_scale, 0 = raw depth). The immersive
// race view sets it so that 1.0 is a chosen near distance and 0 is infinity.
RRV_QUEST_EXPORT void rrv_pcsx2_gs_bridge_android_set_stereo_depth_scale(float scale)
{
    GSStereo::depth_scale.store(scale, std::memory_order_relaxed);
}

// Presentation aspect: 1 = stretch (the immersive race fills each eye), 0 = 16:9 letterboxed in
// the eye's 4:3 half (the fixed screen shows the half undistorted, menus included).
// 0 when the last vsync skipped its field (RRV_PCSX2_GS_FRAMESKIP): its head pose was not shown.
RRV_QUEST_EXPORT int rrv_pcsx2_gs_bridge_android_vsync_presented()
{
    return rrv_quest_hud::vsync_presented.load(std::memory_order_relaxed);
}

RRV_QUEST_EXPORT void rrv_pcsx2_gs_bridge_android_set_present_aspect(int stretch)
{
    rrv_quest_hud::present_aspect.store(stretch != 0 ? 1 : 0, std::memory_order_relaxed);
}

// Quest VR HUD layer (Wipeout Rewrite VR style). rows = the strip at the bottom of the surface the
// HUD is drawn into (0 = none; the eyes take the rows above it). split = take the HUD out of the
// race picture into that strip (immersive view); 0 leaves the HUD in the picture.
RRV_QUEST_EXPORT void rrv_pcsx2_gs_bridge_android_set_hud_layer(int split, int rows)
{
    GSStereo::hud_rows.store(rows > 0 ? rows : 0, std::memory_order_relaxed);
    GSStereo::hud_layer.store(split != 0, std::memory_order_relaxed);
}

// Ridge Racer V HUD layer rule (GSStereo::hud_split_z): the split comes at the first far-plane 2D
// draw (all Z >= z) into the displayed buffer after the field's 3D; 0 = GT4's composite rule.
RRV_QUEST_EXPORT void rrv_pcsx2_gs_bridge_android_set_hud_split_z(uint32_t z)
{
    GSStereo::hud_split_z.store(z, std::memory_order_relaxed);
}
