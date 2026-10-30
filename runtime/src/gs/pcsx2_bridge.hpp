#pragma once

// The PCSX2 GS as the renderer (Quest): the GS queue's render side hands GIF packets, field
// transitions and GS-to-host transfers to librrv-pcsx2-gs-bridge.so (the Ridge Racer V port's
// PCSX2 2.8 / ARMSX2 hardware GS, C ABI in rrv_pcsx2_gs_bridge.h) instead of rasterizing them
// itself. PCSX2 renders with Vulkan straight into the host's window (on the Quest the ANativeWindow
// of an OpenXR Android-surface swapchain), so no frame is copied through the CPU.
//
// Every call except active() runs on the render thread, the one that drains the GS queue.

#include <cstdint>
#include <string>

namespace ps2::gs::bridge {

inline constexpr int kPrivRegCount = 19;

// Payload of a queue::kVblank item: the privileged registers the field is shown with, in the
// bridge's order (PMODE, SMODE1, SMODE2, SRFSH, SYNCH1, SYNCH2, SYNCV, DISPFB1, DISPLAY1, DISPFB2,
// DISPLAY2, EXTBUF, EXTDATA, EXTWRITE, BGCOLOR, CSR, IMR, BUSDIR, SIGLBLID), and the field parity.
struct Field {
    std::uint64_t regs[kPrivRegCount];
    std::uint32_t parity;
    std::uint32_t pad;
};

// Payload of a queue::kDownload item while the bridge renders.
struct Download {
    std::uintptr_t target;
    std::uint64_t bitbltbuf, trxpos, trxreg;
};

// True once open() succeeded; safe from any thread.
bool active();

// Loads the library (PS2_PCSX2_BRIDGE, default librrv-pcsx2-gs-bridge.so) and creates the GS
// rendering into `window` (an ANativeWindow* on Android). Scale etc. come from the bridge's own
// RRV_PCSX2_GS_* environment variables.
bool open(void *window, std::uint32_t width, std::uint32_t height, std::string &error);
void close();

void submit(int path, const std::uint8_t *data, std::uint32_t bytes);
void vsync(const Field &field);
void download(const Download &request, std::uint32_t bytes);
void reset();

// Fields presented, and the bridge's own counters, for the host's perf line.
std::uint64_t fields();

}
