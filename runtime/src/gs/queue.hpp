#pragma once

#include <atomic>
#if !defined(__EMSCRIPTEN__) && !defined(__linux__)
#include <condition_variable>
#include <mutex>
#endif
#include <cstddef>
#include <cstdint>
#include <memory>

namespace ps2::gs::queue {

enum Type : std::uint32_t { kWrap, kGif, kGifReset, kReset, kSoftReset, kVblank, kPresent, kDownload, kVif1, kVif1Write, kVuReset, kState, kVuFence, kDownloadAsync, kPath3Idle, kGifDefer };

enum class Role { Emulation, Vu, Render };

struct Header {
    std::uint32_t type, bytes, arg0, arg1;
};

class Ring {
public:
    void start(std::size_t capacity);
    bool active() const { return capacity_ != 0; }
    void post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes);
    bool peek(Header &header, const std::uint8_t *&payload);
    void pop(const Header &header);
    void wait_for_work(int timeout_ms);
    void wake();

private:
    std::unique_ptr<std::uint8_t[]> buffer_;
    std::uint64_t capacity_ = 0;
    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::atomic<std::uint64_t> tail_{0};
    alignas(64) std::atomic<std::uint32_t> posted_{0};
    alignas(64) std::atomic<std::uint32_t> freed_{0};
    std::atomic<std::uint32_t> consumer_waiting_{0}, producer_waiting_{0};
#if !defined(__EMSCRIPTEN__) && !defined(__linux__)
    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
#endif
};

Ring &render();
Ring &vu();
Role &role();
Ring &outbox();

bool active();
bool vu_active();
void post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes);

void wait(std::atomic<std::uint32_t> &value, std::uint32_t seen);
double waited_ms(Role role);
void notify(std::atomic<std::uint32_t> &value);

}
