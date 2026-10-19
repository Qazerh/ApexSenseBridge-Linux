#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace asb::platform::linux_backend {

// Event-loop producers and one I/O worker. A busy/full queue drops
// diagnostics rather than waiting on a filesystem, terminal or worker lock.
class AsyncDiagnostics {
public:
    AsyncDiagnostics();
    ~AsyncDiagnostics();
    AsyncDiagnostics(const AsyncDiagnostics&) = delete;
    AsyncDiagnostics& operator=(const AsyncDiagnostics&) = delete;
    bool submit(std::function<void()> task);
    // Startup/reconnect state transitions only; never used in the active input path.
    void submitRequired(std::function<void()> task);
    bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }
    std::string error() const;
    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    void finish(); // Drain/join only after input stops and hardware cleanup runs.
private:
    void run();
    static constexpr std::size_t kCapacity = 256;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::array<std::function<void()>, kCapacity> queue_;
    std::size_t head_ = 0, tail_ = 0, size_ = 0;
    bool stopping_ = false;
    std::atomic_bool failed_{false};
    std::atomic_uint64_t dropped_{0};
    std::string error_;
    std::thread worker_;
};

} // namespace asb::platform::linux_backend
