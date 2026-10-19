#pragma once

#include "dualsense/DualSenseFeedback.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace asb::platform::linux_backend {

// Kernel FF_RUMBLE for the standard Apex 6. Uploads/writes never run on the
// input thread. A one-second effect expiry also bounds rumble after a crash.
class EvdevRumble {
public:
    using Writer = std::function<bool(std::uint16_t, std::uint16_t, std::string&)>;
    EvdevRumble(int fd, unsigned strength);
    EvdevRumble(Writer writer, unsigned strength);
    ~EvdevRumble();
    void handle(const dualsense::DualSenseFeedback& feedback);
    void set(std::uint8_t left, std::uint8_t right);
    bool stop(std::string& error);
    bool failed() const { return failed_.load(std::memory_order_acquire); }
    std::string error() const;
    std::uint64_t writes() const { return writes_.load(); }
    std::uint64_t updates() const { return updates_.load(); }
private:
    bool write(std::uint16_t left, std::uint16_t right, std::string& error);
    void run();
    int fd_ = -1, effect_ = -1;
    Writer writer_;
    unsigned strength_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false, pending_ = false, stopped_ = false;
    std::uint16_t left_ = 0, right_ = 0;
    std::string error_;
    std::atomic_bool failed_{false};
    std::atomic_uint64_t writes_{0}, updates_{0};
    std::thread worker_;
};

} // namespace asb::platform::linux_backend
