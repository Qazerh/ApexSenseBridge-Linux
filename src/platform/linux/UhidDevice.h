#pragma once

#include "platform/linux/DualSenseReports.h"

#include <functional>
#include <string>
#include <string_view>
#include <linux/uhid.h>

namespace asb::platform::linux_backend {

// One event loop owns the fd and the feedback callback (no worker races).
class UhidDevice {
public:
    using Handler = std::function<void(const dualsense::DualSenseFeedback&)>;
    using TraceHandler = std::function<void(std::string_view)>;
    UhidDevice() = default;
    // Takes ownership; also allows socketpair-based tests of the UHID wire API.
    explicit UhidDevice(int fd) : fd_(fd) {}
    ~UhidDevice();
    UhidDevice(const UhidDevice&) = delete;
    UhidDevice& operator=(const UhidDevice&) = delete;
    bool create(const std::array<std::uint8_t, 6>& address, std::string& error);
    bool dispatch(const Handler& handler, std::string& error, unsigned budget = 64);
    bool update(const dualsense::DualSenseInputState& state, std::string& error);
    void close() noexcept;
    bool started() const noexcept { return started_; }
    std::uint64_t outputs() const noexcept { return outputs_; }
    int nativeHandle() const noexcept { return fd_; }
    void setTraceHandler(TraceHandler handler) { trace_ = std::move(handler); }
private:
    bool send(const uhid_event& event, std::string& error);
    bool sendBytes(const void* data, std::size_t size, std::string& error);
    void trace(std::string_view message);
    int fd_ = -1;
    bool started_ = false;
    std::uint8_t sequence_ = 0;
    std::uint64_t outputs_ = 0;
    std::array<std::uint8_t, 6> address_{};
    std::array<std::uint8_t, 11> leftEffect_{}, rightEffect_{};
    std::uint32_t hostTimestamp_ = 0;
    std::array<std::uint8_t, 64> inputReport_ = buildUsbInput({}, 0, 0);
    std::array<std::uint8_t, 48> outputReport_{2};
    std::array<std::uint8_t, 48> hostSetupReport_{8};
    TraceHandler trace_;
    std::size_t traceLines_ = 0;
};

} // namespace asb::platform::linux_backend
