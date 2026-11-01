// SPDX-License-Identifier: GPL-3.0-or-later
// The PS2 network adapter: SPEED (expansion bay interface) and SMAP (Ethernet). Ported from
// PCSX2's DEV9 (pcsx2/DEV9/DEV9.cpp, smap.cpp; GPL-3.0+, PCSX2 Dev Team), Ethernet only: no
// hard disk, no flash. Frames go to and come from net::link (the ad hoc relay).
#include "dev9.hpp"

#include "hw.hpp"
#include "iop.hpp"
#include "kernel.hpp"

#include "../net/link.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ps2::iop::dev9 {

namespace {

constexpr std::uint32_t kSpdBase = 0x10000000u;
constexpr std::uint32_t kSmapBase = kSpdBase + 0x100u;
constexpr std::uint32_t kDev9Rev = 0x1F80146Eu;
constexpr unsigned kDev9Line = 13;

// SPEED registers
constexpr std::uint32_t kSpdRev1 = kSpdBase + 0x00, kSpdRev2 = kSpdBase + 0x02, kSpdRev3 = kSpdBase + 0x04;
constexpr std::uint32_t kSpdR0e = kSpdBase + 0x0e, kSpdR20 = kSpdBase + 0x20;
constexpr std::uint32_t kSpdDmaCtrl = kSpdBase + 0x24, kSpdIntrStat = kSpdBase + 0x28, kSpdIntrMask = kSpdBase + 0x2a;
constexpr std::uint32_t kSpdPioDir = kSpdBase + 0x2c, kSpdPioData = kSpdBase + 0x2e;
constexpr std::uint32_t kSpdXfrCtrl = kSpdBase + 0x32, kSpdIfCtrl = kSpdBase + 0x64;
constexpr std::uint16_t kCapsSmap = 1u << 0, kCapsAta = 1u << 1;
constexpr std::uint16_t kDmaToSmap = 1u << 0;

// SMAP registers
constexpr std::uint32_t kSmapBdMode = kSmapBase + 0x02, kSmapIntrClr = kSmapBase + 0x28;
constexpr std::uint32_t kTxFifoCtrl = kSmapBase + 0xf00, kTxFifoWrPtr = kSmapBase + 0xf04, kTxFifoSize = kSmapBase + 0xf08;
constexpr std::uint32_t kTxFifoFrameCnt = kSmapBase + 0xf0c, kTxFifoFrameInc = kSmapBase + 0xf10;
constexpr std::uint32_t kTxFifoData = kSmapBase + 0x1000;
constexpr std::uint32_t kRxFifoCtrl = kSmapBase + 0xf30, kRxFifoRdPtr = kSmapBase + 0xf34, kRxFifoSize = kSmapBase + 0xf38;
constexpr std::uint32_t kRxFifoFrameCnt = kSmapBase + 0xf3c, kRxFifoFrameDec = kSmapBase + 0xf40;
constexpr std::uint32_t kRxFifoData = kSmapBase + 0x1100;
constexpr std::uint8_t kFifoReset = 1u << 0, kFifoDmaEn = 1u << 1;
constexpr std::uint32_t kEmac3Base = kSmapBase + 0x1f00, kEmac3End = kEmac3Base + 0x6c + 4;
constexpr std::uint32_t kEmac3Mode0 = kEmac3Base + 0x00, kEmac3TxMode0 = kEmac3Base + 0x08;
constexpr std::uint32_t kEmac3StaCtrl = kEmac3Base + 0x5c;
constexpr std::uint32_t kE3SoftReset = 1u << 29, kE3TxIdle = 1u << 30, kE3RxIdle = 1u << 31;
constexpr std::uint32_t kE3TxGnp0 = 1u << 31;
constexpr std::uint32_t kE3PhyOpComp = 1u << 15, kE3PhyRead = 1u << 12, kE3PhyWrite = 2u << 12;
constexpr std::uint32_t kBdTxBase = kSmapBase + 0x2f00, kBdRxBase = kSmapBase + 0x3100, kBdSize = 512;
constexpr std::uint16_t kBdTxReady = 1u << 15, kBdRxEmpty = 1u << 15;
constexpr std::uint16_t kIntrRxEnd = 1u << 5, kIntrTxEnd = 1u << 4, kIntrTxDnv = 1u << 2;

// Tick that moves received frames into the RX FIFO and keeps the (level) interrupt asserted.
constexpr std::uint64_t kTickCycles = 36864u / 2u;  // 0.5 ms of IOP time

struct Bd {
    std::uint16_t ctrl_stat, reserved, length, pointer;
};

struct State {
    std::uint8_t regs[0x10000];
    std::uint16_t eeprom[32];
    std::uint8_t eeprom_state, eeprom_command, eeprom_address, eeprom_bit, eeprom_dir;
    std::uint32_t rxbdi, txbdi;
    std::uint8_t rxfifo[16384], txfifo[16384];
    std::uint16_t rxfifo_wr_ptr;
    std::uint8_t bd_swap;
    std::uint16_t phyregs[32];
    std::uint16_t irqcause, irqmask, dma_ctrl, xfr_ctrl, if_ctrl;
};
State g;
bool g_present = false;
std::uint64_t g_last_tick = 0;  // IOP time of the last tick; a stale one re-arms it
std::uint32_t g_tick_chain = 0;  // only the newest tick chain keeps running
std::uint64_t g_tx_frames = 0, g_rx_frames = 0;
// PS2_DEV9_TRACE=1: log every register access (driver bring-up debugging).
const bool g_trace = [] { const char *v = std::getenv("PS2_DEV9_TRACE"); return v && *v == '1'; }();

template <class T> T &reg(std::uint32_t a) { return *reinterpret_cast<T *>(&g.regs[a & 0xFFFFu]); }
Bd *bd(std::uint32_t base, std::uint32_t index) {
    return reinterpret_cast<Bd *>(&g.regs[base & 0xFFFFu]) + index;
}

void update_irq() {
    if (g.irqcause & g.irqmask) hw::raise_interrupt(kDev9Line);
}

void irq(std::uint16_t cause) {
    g.irqcause |= cause;
    update_irq();
}

void ad_reset() {}

bool rx_fifo_can_rx() {
    if (reg<std::uint8_t>(kRxFifoFrameCnt) == 64) return false;
    const int rd = static_cast<int>(reg<std::uint32_t>(kRxFifoRdPtr));
    int space = static_cast<int>(sizeof g.rxfifo) - ((g.rxfifo_wr_ptr - rd) & 16383);
    if (space == 0) space = sizeof g.rxfifo;
    return space >= 1514;
}

void rx_process(const std::vector<std::uint8_t> &frame) {
    Bd *pbd = bd(kBdRxBase, g.rxbdi);
    const std::size_t bytes = (frame.size() + 3) & ~std::size_t{3};
    if (!(pbd->ctrl_stat & kBdRxEmpty)) {
        std::fprintf(stderr, "[dev9] dropping %zu bytes (RX BD %u not ready)\n", bytes, g.rxbdi);
        return;
    }
    const std::uint16_t start = g.rxfifo_wr_ptr & 16383;
    for (std::size_t i = 0; i < bytes; ++i) {
        g.rxfifo[g.rxfifo_wr_ptr] = i < frame.size() ? frame[i] : 0;
        g.rxfifo_wr_ptr = (g.rxfifo_wr_ptr + 1) & 16383;
    }
    g.rxbdi = (g.rxbdi + 1) & (kBdSize / 8 - 1);
    pbd->length = static_cast<std::uint16_t>(frame.size());
    pbd->pointer = static_cast<std::uint16_t>(0x4000 + start);
    pbd->ctrl_stat &= ~kBdRxEmpty;
    ++reg<std::uint8_t>(kRxFifoFrameCnt);
    ++g_rx_frames;
}

bool accepts(const std::vector<std::uint8_t> &frame) {
    if (frame.size() < 14) return false;
    if (frame[0] & 1u) return true;  // broadcast / multicast
    const net::Mac m = net::mac();
    return std::memcmp(frame.data(), m.data(), 6) == 0;
}

void tick(std::uint32_t chain) {
    if (chain != g_tick_chain) return;
    std::vector<std::uint8_t> frame;
    bool got = false;
    while (rx_fifo_can_rx() && net::receive(frame)) {
        if (!accepts(frame)) continue;
        if (frame.size() < 60) frame.resize(60, 0);
        rx_process(frame);
        got = true;
    }
    if (got) g.irqcause |= kIntrRxEnd;
    update_irq();
    g_last_tick = kernel::now();
    kernel::schedule(g_last_tick + kTickCycles, kernel::Job{kernel::kJobDev9Tick, chain});
}

void tx_process() {
    std::uint32_t cnt = 0;
    std::vector<std::uint8_t> frame;
    for (;;) {
        Bd *pbd = bd(kBdTxBase, g.txbdi);
        if (!(pbd->ctrl_stat & kBdTxReady)) break;
        if (pbd->length > 1514) {
            std::fprintf(stderr, "[dev9] TX frame too big (%u)\n", pbd->length);
        } else {
            const std::uint32_t base = (pbd->pointer - 0x1000u) & 16383u;
            const std::uint32_t len = pbd->length;
            // The SMAP driver's adapter detection frame: the real chip reports RXEND (the frame
            // comes back on the loopback) and TXDNV; PCSX2 fakes that the same way.
            if (len == 0x5EA && pbd->pointer == 0x1000) {
                const auto *p = reinterpret_cast<const std::uint32_t *>(&g.txfifo[base]);
                bool test = true;
                for (std::uint32_t i = 0; i < 0x5EA; i += 4)
                    if (*p++ != i) { test = false; break; }
                if (test) irq(kIntrRxEnd | kIntrTxDnv);
            }
            frame.resize(len);
            if (base + len > 16384u) {
                const std::uint32_t was = 16384u - base;
                std::memcpy(frame.data(), g.txfifo + base, was);
                std::memcpy(frame.data() + was, g.txfifo, len - was);
            } else {
                std::memcpy(frame.data(), g.txfifo + base, len);
            }
            if (frame.size() < 60) frame.resize(60, 0);
            net::send(frame.data(), frame.size());
            ++g_tx_frames;
        }
        pbd->ctrl_stat &= ~kBdTxReady;
        g.txbdi = (g.txbdi + 1) & (kBdSize / 8 - 1);
        --reg<std::uint8_t>(kTxFifoFrameCnt);
        ++cnt;
    }
    irq(cnt ? kIntrTxEnd : kIntrTxDnv);
}

std::uint32_t wswap(std::uint32_t d) { return (d >> 16) | (d << 16); }

void emac3_write(std::uint32_t addr) {
    std::uint32_t value = wswap(reg<std::uint32_t>(addr));
    switch (addr) {
    case kEmac3Mode0:
        value = (value & ~kE3SoftReset) | kE3TxIdle | kE3RxIdle;
        reg<std::uint16_t>(kEmac3StaCtrl + 2) |= kE3PhyOpComp;
        break;
    case kEmac3TxMode0:
        tx_process();
        value &= ~kE3TxGnp0;
        break;
    case kEmac3StaCtrl: {
        if (value & kE3PhyRead) {
            value |= kE3PhyOpComp;
            const unsigned r = value & 0x1Fu;
            std::uint16_t v = g.phyregs[r];
            if (r == 0x01) v |= (1u << 2) | (1u << 5);                        // BMSR: link, autoneg done
            if (r == 0x10) v |= (1u << 0) | (1u << 2) | (1u << 4);            // PHYSTS: link, 100M FDX, ANCP
            value = (value & 0xFFFFu) | (static_cast<std::uint32_t>(v) << 16);
        }
        if (value & kE3PhyWrite) {
            value |= kE3PhyOpComp;
            const unsigned r = value & 0x1Fu;
            std::uint16_t v = static_cast<std::uint16_t>(value >> 16);
            if (r == 0x00) {
                if (v & (1u << 15)) ad_reset();
                v = static_cast<std::uint16_t>((v & ~(1u << 15)) | 1u);
            }
            g.phyregs[r] = v;
        }
        break;
    }
    default: break;
    }
    reg<std::uint32_t>(addr) = wswap(value);
}

// ---- SPEED ----

std::uint16_t speed_read(std::uint32_t addr) {
    switch (addr) {
    case kSpdR20: return 1;
    case kSpdIntrStat: return g.irqcause;
    case kSpdIntrMask: return g.irqmask;
    case kSpdPioData: {
        std::uint16_t hard = 0;
        if (g.eeprom_state == 9 && g.eeprom_command == 2) {
            if (g.eeprom_bit != 0xFF)
                hard = static_cast<std::uint16_t>(((g.eeprom[g.eeprom_address & 31] << g.eeprom_bit) & 0x8000) >> 11);
            if (++g.eeprom_bit == 16) {
                ++g.eeprom_address;
                g.eeprom_bit = 0;
            }
        }
        return hard;
    }
    case kSpdRev1: return 0;
    case kSpdRev2: return 0x11;
    case kSpdRev3: return kCapsSmap | kCapsAta;
    case kSpdR0e: return 0x0002;
    case kSpdXfrCtrl: return g.xfr_ctrl;
    case kSpdIfCtrl: return g.if_ctrl;
    default: return reg<std::uint16_t>(addr);
    }
}

void speed_write(std::uint32_t addr, std::uint16_t value) {
    switch (addr) {
    case kSpdR20: return;
    case kSpdIntrStat: g.irqcause = value; return;
    case kSpdIntrMask:
        g.irqmask = value;
        update_irq();
        return;
    case kSpdPioDir:
        if ((value & 0xc0) != 0xc0) return;
        if ((value & 0x30) == 0x20) g.eeprom_state = 0;
        g.eeprom_dir = (value >> 4) & 3;
        return;
    case kSpdPioData:
        if ((value & 0xc0) != 0xc0) return;
        switch (g.eeprom_state) {
        case 0: g.eeprom_command = 0; ++g.eeprom_state; break;   // READY
        case 1:                                                   // OPCD0
            g.eeprom_command = (value >> 4) & 2;
            ++g.eeprom_state;
            g.eeprom_bit = 0xFF;
            break;
        case 2: g.eeprom_command |= (value >> 5) & 1; ++g.eeprom_state; break;  // OPCD1
        case 3: case 4: case 5: case 6: case 7: case 8: {        // ADDR0..5
            const unsigned s = g.eeprom_state - 3u;
            g.eeprom_address = static_cast<std::uint8_t>((g.eeprom_address & (63 ^ (1 << s))) |
                                                         ((value >> s) & (0x20 >> s)));
            ++g.eeprom_state;
            break;
        }
        default: break;  // TDATA: the EEPROM stays read-only
        }
        return;
    case kSpdDmaCtrl: g.dma_ctrl = value; return;
    case kSpdXfrCtrl: g.xfr_ctrl = value; return;
    case kSpdIfCtrl: g.if_ctrl = value; return;
    default: reg<std::uint16_t>(addr) = value; return;
    }
}

// ---- SMAP ----

std::uint16_t smap_read16(std::uint32_t addr) {
    const std::uint16_t rv = reg<std::uint16_t>(addr);
    if ((addr >= kBdTxBase && addr < kBdTxBase + kBdSize) || (addr >= kBdRxBase && addr < kBdRxBase + kBdSize))
        return g.bd_swap ? static_cast<std::uint16_t>((rv << 8) | (rv >> 8)) : rv;
    return rv;
}

std::uint32_t smap_read32(std::uint32_t addr) {
    if (addr >= kEmac3Base && addr < kEmac3End)
        return smap_read16(addr) | (static_cast<std::uint32_t>(smap_read16(addr + 2)) << 16);
    if (addr == kRxFifoData) {
        const std::uint32_t rd = reg<std::uint32_t>(kRxFifoRdPtr) & 16383u;
        std::uint32_t v;
        std::memcpy(&v, g.rxfifo + rd, 4);
        reg<std::uint32_t>(kRxFifoRdPtr) = (rd + 4) & 16383u;
        return v;
    }
    return reg<std::uint32_t>(addr);
}

void smap_write8(std::uint32_t addr, std::uint8_t value) {
    switch (addr) {
    case kTxFifoFrameInc: ++reg<std::uint8_t>(kTxFifoFrameCnt); return;
    case kRxFifoFrameDec:
        reg<std::uint8_t>(addr) = value;
        --reg<std::uint8_t>(kRxFifoFrameCnt);
        return;
    case kTxFifoCtrl:
        if (value & kFifoReset) {
            g.txbdi = 0;
            reg<std::uint8_t>(kTxFifoFrameCnt) = 0;
            reg<std::uint32_t>(kTxFifoWrPtr) = 0;
            reg<std::uint32_t>(kTxFifoSize) = 16384;
        }
        reg<std::uint8_t>(addr) = value & ~kFifoReset;
        return;
    case kRxFifoCtrl:
        if (value & kFifoReset) {
            g.rxbdi = 0;
            g.rxfifo_wr_ptr = 0;
            reg<std::uint8_t>(kRxFifoFrameCnt) = 0;
            reg<std::uint32_t>(kRxFifoRdPtr) = 0;
            reg<std::uint32_t>(kRxFifoSize) = 16384;
        }
        reg<std::uint8_t>(addr) = value & ~kFifoReset;
        return;
    case kSmapBdMode: g.bd_swap = value & 1u; return;
    default: reg<std::uint8_t>(addr) = value; return;
    }
}

void smap_write16(std::uint32_t addr, std::uint16_t value) {
    if ((addr >= kBdTxBase && addr < kBdTxBase + kBdSize) || (addr >= kBdRxBase && addr < kBdRxBase + kBdSize)) {
        if (g.bd_swap) value = static_cast<std::uint16_t>((value >> 8) | (value << 8));
        reg<std::uint16_t>(addr) = value;
        return;
    }
    if (addr == kSmapIntrClr) {
        g.irqcause &= ~value;
        return;
    }
    reg<std::uint16_t>(addr) = value;
    // EMAC3 registers are 32-bit, written low half then high half: act on the high half.
    if (addr >= kEmac3Base && addr < kEmac3End && (addr & 3u) == 2u) emac3_write(addr - 2);
}

void smap_write32(std::uint32_t addr, std::uint32_t value) {
    if (addr >= kEmac3Base && addr < kEmac3End) {
        smap_write16(addr, static_cast<std::uint16_t>(value));
        smap_write16(addr + 2, static_cast<std::uint16_t>(value >> 16));
        return;
    }
    if (addr == kTxFifoData) {
        const std::uint32_t wr = reg<std::uint32_t>(kTxFifoWrPtr) & 16383u;
        std::memcpy(g.txfifo + wr, &value, 4);
        reg<std::uint32_t>(kTxFifoWrPtr) = (wr + 4) & 16383u;
        return;
    }
    reg<std::uint32_t>(addr) = value;
}

}

void reset() {
    std::memset(&g, 0, sizeof g);
    g_present = net::enabled();
    kernel::register_job(kernel::kJobDev9Tick, [](const kernel::Job &j) { tick(j.a); });
    if (!g_present) return;
    // EEPROM: the MAC in words 0-2, their 16-bit sum as checksum in word 3 (as PCSX2 / real units).
    const net::Mac m = net::mac();
    std::memcpy(g.eeprom, m.data(), 6);
    g.eeprom[3] = static_cast<std::uint16_t>(g.eeprom[0] + g.eeprom[1] + g.eeprom[2]);
    for (std::uint32_t i = 0; i < kBdSize / 8; ++i) bd(kBdRxBase, i)->ctrl_stat = kBdRxEmpty;
    reg<std::uint32_t>(kTxFifoSize) = 16384;
    reg<std::uint32_t>(kRxFifoSize) = 16384;
    net::start();
    g_last_tick = 0;
}

bool present() { return g_present; }

bool owns(std::uint32_t p) { return g_present && ((p >= kSpdBase && p < kSpdBase + 0x10000u) || p == kDev9Rev); }

std::uint32_t read_impl(std::uint32_t a, unsigned size);

std::uint32_t read(std::uint32_t a, unsigned size) {
    const std::uint32_t v = read_impl(a, size);
    if (g_trace) std::fprintf(stderr, "[dev9] r%u %08X = %X (iop pc %08X)\n", size * 8, a, v, g_cpu.pc);
    return v;
}

std::uint32_t read_impl(std::uint32_t a, unsigned size) {
    if (a == kDev9Rev) return 0x32;  // expansion bay
    // The tick starts with the driver's first access, and again after a save state was loaded
    // without one queued.
    if (g_last_tick == 0 || kernel::now() > g_last_tick + 16 * kTickCycles) {
        g_last_tick = kernel::now();
        kernel::schedule(g_last_tick + kTickCycles, kernel::Job{kernel::kJobDev9Tick, ++g_tick_chain});
    }
    if (a < kSmapBase) {
        if (size == 4) return reg<std::uint32_t>(a);
        const std::uint16_t v = speed_read(a & ~1u);
        return size == 1 ? ((a & 1u) ? v >> 8 : v & 0xFFu) : v;
    }
    switch (size) {
    case 1: return reg<std::uint8_t>(a);
    case 2: return smap_read16(a);
    default: return smap_read32(a);
    }
}

void write(std::uint32_t a, std::uint32_t v, unsigned size) {
    if (g_trace) std::fprintf(stderr, "[dev9] w%u %08X = %X (iop pc %08X)\n", size * 8, a, v, g_cpu.pc);
    if (a == kDev9Rev) return;
    if (a < kSmapBase) {
        if (size == 4) {
            reg<std::uint32_t>(a) = v;
            return;
        }
        speed_write(a, static_cast<std::uint16_t>(v));  // 8-bit writes affect the whole register
        return;
    }
    switch (size) {
    case 1: smap_write8(a, static_cast<std::uint8_t>(v)); break;
    case 2: smap_write16(a, static_cast<std::uint16_t>(v)); break;
    default: smap_write32(a, v); break;
    }
}

void dma(std::uint32_t madr, std::uint32_t bytes, bool to_device) {
    if (!g_present || !(g.dma_ctrl & kDmaToSmap)) return;
    std::uint8_t *mem = ram_ptr(madr);
    if (to_device) {
        if (!(reg<std::uint8_t>(kTxFifoCtrl) & kFifoDmaEn)) return;
        std::uint32_t wr = reg<std::uint32_t>(kTxFifoWrPtr) & 16383u;
        for (std::uint32_t i = 0; i < bytes; i += 4) {
            std::memcpy(g.txfifo + wr, mem + i, 4);
            wr = (wr + 4) & 16383u;
        }
        reg<std::uint32_t>(kTxFifoWrPtr) = wr;
        reg<std::uint8_t>(kTxFifoCtrl) &= ~kFifoDmaEn;
    } else {
        if (!(reg<std::uint8_t>(kRxFifoCtrl) & kFifoDmaEn)) return;
        std::uint32_t rd = reg<std::uint32_t>(kRxFifoRdPtr) & 16383u;
        for (std::uint32_t i = 0; i < bytes; i += 4) {
            std::memcpy(mem + i, g.rxfifo + rd, 4);
            rd = (rd + 4) & 16383u;
        }
        reg<std::uint32_t>(kRxFifoRdPtr) = rd;
        reg<std::uint8_t>(kRxFifoCtrl) &= ~kFifoDmaEn;
    }
}

}
