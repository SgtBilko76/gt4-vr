#pragma once

// The PS2 network adapter (DEV9 expansion bay): the SPEED interface chip and its SMAP Ethernet
// controller. Present only while a network link is configured (net::enabled()); without one the
// IOP sees an empty bay, as before.
#include <cstddef>
#include <cstdint>

namespace ps2::iop::dev9 {

void reset();
bool present();

// SPEED/SMAP registers live at physical 0x10000000..0x1000FFFF; DEV9_R_REV at 0x1F80146E.
bool owns(std::uint32_t phys);
std::uint32_t read(std::uint32_t phys, unsigned size);
void write(std::uint32_t phys, std::uint32_t value, unsigned size);

// IOP DMA channel 8: `bytes` between IOP RAM at `madr` and the SMAP FIFOs.
void dma(std::uint32_t madr, std::uint32_t bytes, bool to_device);

}
