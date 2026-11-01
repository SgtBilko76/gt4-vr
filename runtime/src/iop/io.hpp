#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ps2::state {
class Stream;
}

namespace ps2::iop::io {

void state(ps2::state::Stream &s);

int open(const std::string &path, std::uint32_t flags);
int close(int fd);
int read(int fd, std::uint8_t *out, std::uint32_t size);
int write(int fd, const std::uint8_t *data, std::uint32_t size);
std::int64_t seek(int fd, std::int64_t offset, int whence);
bool stat(const std::string &path, std::uint32_t &mode, std::uint32_t &size);
void reset();

// Devices that IOP modules register with AddDrv (name -> iop_device_t in IOP RAM), the files open
// on them, and per thread the driver calls in flight (io.cpp install_io).
struct DriverFile {
    std::uint32_t file = 0;  // iop_file_t in IOP RAM
    bool dir = false;
};
struct Pending {
    std::uint32_t ra = 0, file = 0;
    std::int32_t fd = -1;
    std::uint32_t kind = 0;
    std::uint32_t stack = 0;  // bytes the call took off sp for stack arguments
};
extern std::map<std::string, std::uint32_t> g_drivers;
extern std::map<int, DriverFile> g_driver_files;
extern std::map<int, std::vector<Pending>> g_pending;
int allocate_fd();

}
