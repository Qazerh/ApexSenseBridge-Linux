#pragma once

#include "platform/linux/UhidDevice.h"
#include "flydigi/Apex5Device.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace asb::platform::linux_backend {

class ControllerUnavailable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A hidraw number can change on replug. Keep following the verified physical
// controller by serial or USB location, never whichever pad gets its old number.
class ControllerSelector {
public:
    explicit ControllerSelector(std::string path) : path_(std::move(path)) {}
    std::optional<HidDeviceInfo> select(const std::vector<HidDeviceInfo>& candidates) const;
    void remember(const HidDeviceInfo& device, std::uint8_t model);
private:
    std::string path_;
    std::optional<HidDeviceInfo> device_;
    std::optional<std::uint8_t> model_;
};

// Persistent game effects survive a hardware reconnect. Rumble is transient:
// it is deliberately not replayed after a pause. One event-loop owner at a time.
class FeedbackState {
public:
    void remember(const dualsense::DualSenseFeedback& feedback);
    dualsense::DualSenseFeedback snapshot() const { return state_; }
private:
    dualsense::DualSenseFeedback state_;
};

// Only runs while hardware is absent/initializing/cleaning up. The active input
// pump has sole ownership again after stop() joins this thread.
class IdlePad {
public:
    IdlePad(UhidDevice& pad, FeedbackState& feedback) : pad_(pad), feedback_(feedback) {}
    ~IdlePad() { stop(); }
    void start();
    void stop();
    bool failed() const { return failed_.load(std::memory_order_acquire); }
    std::string error() const;
private:
    void run() noexcept;
    UhidDevice& pad_;
    FeedbackState& feedback_;
    std::atomic_bool stopping_{false}, failed_{false};
    std::thread worker_;
    mutable std::mutex mutex_;
    std::string error_;
};

// Preserve the original settings across a failed cleanup, so reconnect does not
// mistake a bridge-selected profile or live RGB frame for the user's baseline.
struct DeferredSettings {
    bool pending = false;
    std::uint8_t model = 0, originalSlot = 0, rgbSlot = 0;
    flydigi::InputTransportStatus transport;
    std::optional<std::array<std::uint8_t, flydigi::kRgbConfigSize>> rgb;
    bool recover(flydigi::Apex5Device& device, std::string& error);
};

} // namespace asb::platform::linux_backend
