#include "io.hpp"
#include "kernel.hpp"

#include "ps2/disc.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <map>

namespace ps2::iop::io {

namespace {

constexpr int kNoEntry = -2;
constexpr int kBadFd = -9;
constexpr int kTooMany = -24;

struct File {
    std::uint32_t sector = 0;
    std::uint32_t size = 0;
    std::uint64_t position = 0;
    std::vector<std::uint8_t> memory;
    bool in_memory = false;
};

std::map<int, File> g_files;

}

// Devices of IOP drivers (AddDrv) and their open files: see install_io.
std::map<std::string, std::uint32_t> g_drivers;
std::map<int, DriverFile> g_driver_files;
std::map<int, std::vector<Pending>> g_pending;

int allocate_fd() {
    for (int fd = 3; fd < 64; ++fd)
        if (!g_files.count(fd) && !g_driver_files.count(fd)) return fd;
    return -24;
}

namespace {

std::string device_of(const std::string &path) {
    const auto colon = path.find(':');
    std::string d = colon == std::string::npos ? "" : path.substr(0, colon);
    for (char &ch : d) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    while (!d.empty() && std::isdigit(static_cast<unsigned char>(d.back()))) d.pop_back();
    return d;
}

int allocate() { return allocate_fd(); }

}

int open(const std::string &path, std::uint32_t flags) {
    const std::string device = device_of(path);
    File f;
    if (device == "cdrom") {
        const auto entry = disc::find(path);
        if (!entry || entry->directory) return kNoEntry;
        f.sector = entry->sector;
        f.size = entry->size;
    } else if (device == "rom") {
        std::string name = path.substr(path.find(':') + 1);
        if (name == "ROMVER") {
            const std::string text = "0220EC20060210\n";
            f.memory.assign(text.begin(), text.end());
        } else if (name == "OSDSYS" || name.rfind("rom0:", 0) == 0) {
            return kNoEntry;
        } else {
            return kNoEntry;
        }
        f.in_memory = true;
        f.size = static_cast<std::uint32_t>(f.memory.size());
    } else {
        std::fprintf(stderr, "[io] open %s (flags 0x%X) on unsupported device\n", path.c_str(), flags);
        return kNoEntry;
    }
    const int fd = allocate();
    if (fd < 0) return fd;
    g_files[fd] = std::move(f);
    if (kernel::g_trace_hle) std::fprintf(stderr, "[io] open %s -> %d\n", path.c_str(), fd);
    return fd;
}

int close(int fd) { return g_files.erase(fd) ? 0 : kBadFd; }

int read(int fd, std::uint8_t *out, std::uint32_t size) {
    auto it = g_files.find(fd);
    if (it == g_files.end()) return kBadFd;
    File &f = it->second;
    if (f.position >= f.size) return 0;
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::uint64_t>(size, f.size - f.position));
    if (f.in_memory) {
        std::memcpy(out, f.memory.data() + f.position, n);
    } else {
        const std::uint64_t start = f.position, end = f.position + n;
        const std::uint32_t first = static_cast<std::uint32_t>(start / disc::kSector);
        const std::uint32_t last = static_cast<std::uint32_t>((end + disc::kSector - 1) / disc::kSector);
        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(last - first) * disc::kSector);
        disc::read(f.sector + first, last - first, buffer.data());
        std::memcpy(out, buffer.data() + (start - static_cast<std::uint64_t>(first) * disc::kSector), n);
    }
    f.position += n;
    return static_cast<int>(n);
}

int write(int fd, const std::uint8_t *, std::uint32_t size) {
    if (fd == 1 || fd == 2) return static_cast<int>(size);
    return g_files.count(fd) ? static_cast<int>(size) : kBadFd;
}

std::int64_t seek(int fd, std::int64_t offset, int whence) {
    auto it = g_files.find(fd);
    if (it == g_files.end()) return kBadFd;
    File &f = it->second;
    std::int64_t base = whence == 1 ? static_cast<std::int64_t>(f.position) : whence == 2 ? f.size : 0;
    const std::int64_t target = std::clamp<std::int64_t>(base + offset, 0, f.size);
    f.position = static_cast<std::uint64_t>(target);
    return target;
}

bool stat(const std::string &path, std::uint32_t &mode, std::uint32_t &size) {
    if (device_of(path) != "cdrom") return false;
    const auto entry = disc::find(path);
    if (!entry) return false;
    mode = entry->directory ? 0x0027u : 0x0017u;
    size = entry->size;
    return true;
}

void reset() {
    g_files.clear();
    g_drivers.clear();
    g_driver_files.clear();
    g_pending.clear();
}

}

namespace ps2::iop::kernel {

namespace {

std::uint32_t arg(const Cpu &c, int i) { return i < 4 ? c.r[4 + i] : read32(c.r[29] + 16u + static_cast<std::uint32_t>(i - 4) * 4u); }

void def(const char *library, std::uint32_t index, const char *name, std::function<void(Cpu &)> body) {
    register_hle(library, index, [library, name, body = std::move(body)](Cpu &c) {
        trace_hle(library, name, c);
        body(c);
    });
}

}

namespace {

// ---- devices registered by IOP modules (AddDrv), e.g. MCMAN's "mc" ----
// A call on such a device is run by the driver's own function ON THE CALLING THREAD: the HLE
// function points the thread at the driver op with a continuation stub as return address, so
// the driver can block on semaphores and SIO2 interrupts as on hardware. The continuation turns
// the op's result into ioman's (fd for open/dopen) and frees the iop_file_t.
// iop_device_t: name, type, version, desc, ops; ops: init, deinit, format, open, close, read,
// write, lseek, ioctl, remove, mkdir, rmdir, dopen, dclose, dread, getstat, chstat, [FSEXT:]
// rename, chdir, sync, mount, umount, lseek64, devctl. iop_file_t: mode, unit, device, privdata.
constexpr std::uint32_t kFsExt = 0x10000000u;
enum Op : std::uint32_t {
    kOpFormat = 2, kOpOpen, kOpClose, kOpRead, kOpWrite, kOpLseek, kOpIoctl, kOpRemove, kOpMkdir, kOpRmdir,
    kOpDopen, kOpDclose, kOpDread, kOpGetstat, kOpChstat, kOpRename, kOpChdir, kOpSync,
    kOpMount, kOpUmount, kOpLseek64, kOpDevctl,
};
enum Kind : std::uint32_t { kKeep, kNewFd, kDropFd, kTempFile };

std::uint32_t g_io_return = 0;

std::string device_name(const std::string &path) {
    const auto colon = path.find(':');
    std::string d = colon == std::string::npos ? "" : path.substr(0, colon);
    for (char &ch : d) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    while (!d.empty() && std::isdigit(static_cast<unsigned char>(d.back()))) d.pop_back();
    return d;
}

int unit_of(const std::string &path) {
    const auto colon = path.find(':');
    if (colon == std::string::npos) return 0;
    std::size_t i = colon;
    while (i > 0 && std::isdigit(static_cast<unsigned char>(path[i - 1]))) --i;
    return i < colon ? std::atoi(path.substr(i, colon - i).c_str()) : 0;
}

std::uint32_t driver_for(const std::string &path) {
    const auto it = io::g_drivers.find(device_name(path));
    return it == io::g_drivers.end() ? 0u : it->second;
}

std::uint32_t new_file(std::uint32_t device, int unit, std::uint32_t mode) {
    const std::uint32_t f = alloc(16);
    if (!f) return 0;
    write32(f, mode);
    write32(f + 4, static_cast<std::uint32_t>(unit));
    write32(f + 8, device);
    write32(f + 12, 0);
    return f;
}

// Sends the calling thread into op `op` of the device; false = the driver has no such op.
// Arguments past the fourth go on the stack (sp+16...): sp is lowered for them and restored by
// the continuation.
bool redirect(Cpu &c, std::uint32_t device, std::uint32_t op, std::uint32_t file, std::uint32_t a1, std::uint32_t a2,
              std::uint32_t a3, Kind kind, int fd, std::initializer_list<std::uint32_t> stack_args = {}) {
    if (op >= kOpRename && !(read32(device + 4) & kFsExt)) return false;
    const std::uint32_t fn = read32(read32(device + 16) + op * 4u);
    Thread *t = current();
    if (!fn || !t) return false;
    std::uint32_t stack = 0;
    if (stack_args.size()) {
        stack = (16u + 4u * static_cast<std::uint32_t>(stack_args.size()) + 7u) & ~7u;
        c.r[29] -= stack;
        std::uint32_t at = c.r[29] + 16u;
        for (const std::uint32_t v : stack_args) {
            write32(at, v);
            at += 4;
        }
    }
    io::g_pending[t->id].push_back(io::Pending{c.pc, file, static_cast<std::int32_t>(fd), kind, stack});
    c.r[4] = file;
    c.r[5] = a1;
    c.r[6] = a2;
    c.r[7] = a3;
    c.r[31] = g_io_return;
    c.pc = fn;
    c.npc = fn + 4;
    return true;
}

void io_return(Cpu &c) {
    Thread *t = current();
    auto it = t ? io::g_pending.find(t->id) : io::g_pending.end();
    if (it == io::g_pending.end() || it->second.empty()) {
        std::fprintf(stderr, "[io] driver call returned with nothing pending\n");
        return;
    }
    const io::Pending p = it->second.back();
    it->second.pop_back();
    if (it->second.empty()) io::g_pending.erase(it);
    const std::int32_t result = static_cast<std::int32_t>(c.r[2]);
    if (g_trace_hle) std::fprintf(stderr, "[io] driver call (kind %u, fd %d) -> %d\n", p.kind, p.fd, result);
    switch (p.kind) {
    case kNewFd:
        if (result < 0) {
            io::g_driver_files.erase(p.fd);
            free(p.file);
        } else {
            c.r[2] = static_cast<std::uint32_t>(p.fd);
        }
        break;
    case kDropFd:
        io::g_driver_files.erase(p.fd);
        free(p.file);
        break;
    case kTempFile: free(p.file); break;
    default: break;
    }
    c.r[29] += p.stack;
    c.r[31] = p.ra;
    c.pc = p.ra;
    c.npc = p.ra + 4;
}

int new_fd(std::uint32_t file, bool dir) {
    const int fd = io::allocate_fd();
    if (fd >= 0) io::g_driver_files[fd] = io::DriverFile{file, dir};
    return fd;
}

std::uint32_t driver_file(int fd) {
    const auto it = io::g_driver_files.find(fd);
    return it == io::g_driver_files.end() ? 0u : it->second.file;
}

// A call by name: open/dopen get a new fd, the others a temporary iop_file_t.
bool by_name(Cpu &c, std::uint32_t op, Kind kind, std::uint32_t mode, std::uint32_t a2, std::uint32_t a3) {
    const std::string path = ram_string(arg(c, 0));
    const std::uint32_t device = driver_for(path);
    if (!device) return false;
    const std::uint32_t file = new_file(device, unit_of(path), mode);
    if (!file) {
        c.r[2] = static_cast<std::uint32_t>(-12);
        return true;
    }
    int fd = -1;
    if (kind == kNewFd && (fd = new_fd(file, op == kOpDopen)) < 0) {
        free(file);
        c.r[2] = static_cast<std::uint32_t>(fd);
        return true;
    }
    const std::uint32_t name = arg(c, 0) + static_cast<std::uint32_t>(path.find(':') + 1);
    if (!redirect(c, device, op, file, name, a2, a3, kind, fd)) {
        if (fd >= 0) io::g_driver_files.erase(fd);
        free(file);
        c.r[2] = static_cast<std::uint32_t>(-48);  // ENOTSUP
    }
    return true;
}

// A call on an open fd of a driver device.
bool by_fd(Cpu &c, std::uint32_t op, Kind kind, std::uint32_t a1, std::uint32_t a2) {
    const int fd = static_cast<int>(arg(c, 0));
    const std::uint32_t file = driver_file(fd);
    if (!file) return false;
    if (!redirect(c, read32(file + 8), op, file, a1, a2, 0, kind, fd)) c.r[2] = static_cast<std::uint32_t>(-48);
    return true;
}

}

void install_io() {
    io::reset();
    g_io_return = hle_stub(io_return);
    register_hle_library("ioman", 0x0104);
    def("ioman", 4, "open", [](Cpu &c) {
        if (by_name(c, kOpOpen, kNewFd, arg(c, 1), arg(c, 1), 0)) return;
        c.r[2] = static_cast<std::uint32_t>(io::open(ram_string(arg(c, 0)), arg(c, 1)));
    });
    def("ioman", 5, "close", [](Cpu &c) {
        if (by_fd(c, kOpClose, kDropFd, 0, 0)) return;
        c.r[2] = static_cast<std::uint32_t>(io::close(static_cast<int>(arg(c, 0))));
    });
    def("ioman", 6, "read", [](Cpu &c) {
        if (by_fd(c, kOpRead, kKeep, arg(c, 1), arg(c, 2))) return;
        std::vector<std::uint8_t> buffer(arg(c, 2));
        const int n = io::read(static_cast<int>(arg(c, 0)), buffer.data(), arg(c, 2));
        for (int i = 0; i < n; ++i) write8(arg(c, 1) + static_cast<std::uint32_t>(i), buffer[static_cast<std::size_t>(i)]);
        c.r[2] = static_cast<std::uint32_t>(n);
    });
    def("ioman", 7, "write", [](Cpu &c) {
        if (by_fd(c, kOpWrite, kKeep, arg(c, 1), arg(c, 2))) return;
        if (arg(c, 0) == 1 || arg(c, 0) == 2) std::fprintf(stderr, "[iop-tty] %s", ram_string(arg(c, 1), arg(c, 2)).c_str());
        c.r[2] = static_cast<std::uint32_t>(io::write(static_cast<int>(arg(c, 0)), nullptr, arg(c, 2)));
    });
    def("ioman", 8, "lseek", [](Cpu &c) {
        if (by_fd(c, kOpLseek, kKeep, arg(c, 1), arg(c, 2))) return;
        c.r[2] = static_cast<std::uint32_t>(io::seek(static_cast<int>(arg(c, 0)), static_cast<std::int32_t>(arg(c, 1)), static_cast<int>(arg(c, 2))));
    });
    def("ioman", 16, "getstat", [](Cpu &c) {
        if (by_name(c, kOpGetstat, kTempFile, 0, arg(c, 1), 0)) return;
        std::uint32_t mode = 0, size = 0;
        if (!io::stat(ram_string(arg(c, 0)), mode, size)) { c.r[2] = static_cast<std::uint32_t>(-2); return; }
        const std::uint32_t p = arg(c, 1);
        std::memset(ram_ptr(p), 0, 40);
        write32(p, mode);
        write32(p + 8, size);
        c.r[2] = 0;
    });
    def("ioman", 9, "ioctl", [](Cpu &c) {
        if (!by_fd(c, kOpIoctl, kKeep, arg(c, 1), arg(c, 2))) c.r[2] = static_cast<std::uint32_t>(-1);
    });
    def("ioman", 10, "remove", [](Cpu &c) {
        if (!by_name(c, kOpRemove, kTempFile, 0, 0, 0)) c.r[2] = static_cast<std::uint32_t>(-1);
    });
    def("ioman", 11, "mkdir", [](Cpu &c) {  // FSEXT mkdir takes a mode too: 0777
        if (!by_name(c, kOpMkdir, kTempFile, 0, 0777, 0)) c.r[2] = static_cast<std::uint32_t>(-1);
    });
    def("ioman", 12, "rmdir", [](Cpu &c) {
        if (!by_name(c, kOpRmdir, kTempFile, 0, 0, 0)) c.r[2] = static_cast<std::uint32_t>(-1);
    });
    def("ioman", 13, "dopen", [](Cpu &c) {
        if (!by_name(c, kOpDopen, kNewFd, 0, 0, 0)) c.r[2] = static_cast<std::uint32_t>(-2);
    });
    def("ioman", 14, "dclose", [](Cpu &c) {
        if (!by_fd(c, kOpDclose, kDropFd, 0, 0)) c.r[2] = static_cast<std::uint32_t>(-9);
    });
    def("ioman", 15, "dread", [](Cpu &c) {
        if (!by_fd(c, kOpDread, kKeep, arg(c, 1), 0)) c.r[2] = static_cast<std::uint32_t>(-9);
    });
    def("ioman", 17, "chstat", [](Cpu &c) {
        if (!by_name(c, kOpChstat, kTempFile, 0, arg(c, 1), arg(c, 2))) c.r[2] = static_cast<std::uint32_t>(-1);
    });
    def("ioman", 18, "format", [](Cpu &c) {
        const std::string path = ram_string(arg(c, 0));
        const std::uint32_t device = driver_for(path);
        const std::uint32_t file = device ? new_file(device, unit_of(path), 0) : 0;
        if (!file || !redirect(c, device, kOpFormat, file, 0, 0, 0, kTempFile, -1)) {
            if (file) free(file);
            c.r[2] = static_cast<std::uint32_t>(-1);
        }
    });
    // AddDrv / DelDrv: record the device (name -> iop_device_t) so calls on it reach its driver.
    // Drivers treat a failed registration as fatal and unload themselves (DEV9.IRX returned
    // NO_RESIDENT_END on -1, and SMAP.IRX was then loaded over its code). dev9x is reached by
    // the EE through the HLE fileio server (sif.cpp devctl) instead.
    def("ioman", 20, "AddDrv", [](Cpu &c) {
        const std::uint32_t device = arg(c, 0);
        const std::string name = ram_string(read32(device), 16);
        io::g_drivers[name] = device;
        std::fprintf(stderr, "[io] device %s: added (type 0x%X)\n", name.c_str(), read32(device + 4));
        c.r[2] = 0;
    });
    def("ioman", 21, "DelDrv", [](Cpu &c) {
        io::g_drivers.erase(ram_string(arg(c, 0), 16));
        c.r[2] = 0;
    });
    def("ioman", 25, "rename", [](Cpu &c) {
        // The new name also loses its "dev:" part, as iomanX does.
        const std::string to = ram_string(arg(c, 1));
        const auto colon = to.find(':');
        const std::uint32_t to_path = arg(c, 1) + (colon == std::string::npos ? 0u : static_cast<std::uint32_t>(colon + 1));
        if (!by_name(c, kOpRename, kTempFile, 0, to_path, 0)) c.r[2] = static_cast<std::uint32_t>(-1);
    });
    def("ioman", 27, "sync", [](Cpu &c) {
        if (!by_name(c, kOpSync, kTempFile, 0, arg(c, 1), 0)) c.r[2] = 0;
    });
    // devctl(name, cmd, arg, arglen, buf, buflen) -> op devctl(file, name, cmd, arg, arglen, buf,
    // buflen). NETCNF asks MCMAN for the card's free space this way before saving.
    def("ioman", 31, "devctl", [](Cpu &c) {
        const std::string path = ram_string(arg(c, 0));
        const std::uint32_t device = driver_for(path);
        const std::uint32_t file = device ? new_file(device, unit_of(path), 0) : 0;
        const std::uint32_t name = arg(c, 0) + static_cast<std::uint32_t>(path.find(':') + 1);
        if (!file || !redirect(c, device, kOpDevctl, file, name, arg(c, 1), arg(c, 2), kTempFile, -1,
                               {arg(c, 3), arg(c, 4), arg(c, 5)})) {
            if (file) free(file);
            c.r[2] = static_cast<std::uint32_t>(-1);
        }
    });
}

}

namespace ps2::iop::io {

void state(ps2::state::Stream &s) {
    s.tag("iopio");
    s.map(g_files, [&s](File &f) {
        s.pod(f.sector);
        s.pod(f.size);
        s.pod(f.position);
        s.vec(f.memory);
        s.pod(f.in_memory);
    });
    if (s.version() >= 7) {
        s.string_map(g_drivers, [&s](std::uint32_t &device) { s.pod(device); });
        s.map(g_driver_files, [&s](DriverFile &f) { s.pod(f); });
        s.map(g_pending, [&s](std::vector<Pending> &v) { s.vec(v, [&s](Pending &p) { s.pod(p); }); });
    } else if (s.loading()) {
        g_drivers.clear();
        g_driver_files.clear();
        g_pending.clear();
    }
}

}
