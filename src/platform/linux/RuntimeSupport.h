#pragma once

#include "platform/linux/Options.h"
#include "flydigi/Apex5Device.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <iosfwd>
#include <thread>

namespace asb::platform::linux_backend {

class InputPipeline {
public:
    InputPipeline(const flydigi::Apex5Identity& identity, const Options& options)
        : identity_(identity), options_(options), gestures_(options.touchpadProfile) {}
    std::optional<dualsense::DualSenseInputState> decode(std::span<const std::uint8_t> packet);
    dualsense::DualSenseInputState render(const dualsense::DualSenseInputState& physical,
                                         dualsense::TouchpadGestureMapper::Clock::time_point now);
    const flydigi::Apex5Identity& identity() const { return identity_; }
    dualsense::TouchpadGestureProfileStats gestureStats() const { return gestures_.stats(); }
private:
    flydigi::Apex5Identity identity_;
    Options options_;
    dualsense::TouchpadGestureMapper gestures_;
};

// Only requests metadata. Replies are observed on InputPipeline's independent queue.
class BatteryRefresh {
public:
    explicit BatteryRefresh(flydigi::Apex5Device& device,
                            std::chrono::milliseconds interval = std::chrono::seconds(30));
    ~BatteryRefresh();
    void stop();
    std::uint64_t failures() const { return failures_.load(); }
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::thread thread_;
    std::atomic_uint64_t failures_{0};
};

struct SessionStatus {
    std::string phase = "starting", error, device, connection, touchpadProfile = "none";
    bool cleanupOk = false;
    unsigned originalProfile = 0, activeProfile = 0, battery = 0, chargeState = 0;
    std::uint64_t elapsedMs = 0, inputs = 0, outputs = 0, translations = 0, unsupported = 0;
    std::uint64_t motion = 0, rgbUpdates = 0, rgbWrites = 0, rgbErrors = 0;
    std::uint64_t rumbleUpdates = 0, rumbleWrites = 0, usbWrites = 0, coalesced = 0, retries = 0;
    std::uint64_t touchTaps = 0, touchSwipes = 0, batteryRefreshFailures = 0;
    std::uint64_t inputForwarded = 0, inputHeartbeats = 0, diagnosticDrops = 0;
    std::string model, feedbackBackend;
    bool batteryAvailable = false, motionSupported = false, rgbSupported = false;
    std::uint64_t hapticFrames = 0, connections = 0;
};

void printPeriodicStatus(std::ostream& out, const SessionStatus& status, bool verbose);
std::string statusJson(const SessionStatus& status);
std::string identityJson(const HidDeviceInfo& info, const flydigi::Apex5Identity& identity);
std::string inputJson(const dualsense::DualSenseInputState& state);
std::string candidatesJson(const std::vector<HidDeviceInfo>& devices);

class StatusFile {
public:
    explicit StatusFile(std::filesystem::path path) : path_(std::move(path)) {}
    SessionStatus status;
    void write() const;
private:
    std::filesystem::path path_;
};

ForceTriggerCommand triggerTestCommand(const Options& options, TriggerSide side);
bool selectProfile(flydigi::Apex5Device& device, std::uint8_t slot, std::string& error);

} // namespace asb::platform::linux_backend
