#pragma once

#include "dualsense/AdaptiveTriggerBridge.h"
#include "dualsense/Apex6HapticBridge.h"
#include "dualsense/RumbleBridge.h"
#include "platform/linux/Options.h"
#include "platform/linux/EvdevRumble.h"

#include <memory>

namespace asb::platform::linux_backend {

// Model selection is made ONLY after verified identity. Vendor protocols are
// never inferred from an evdev name or shared VID/PID alone.
class FeedbackBridge {
public:
    FeedbackBridge(flydigi::Apex5Device& device, const Options& options);
    ~FeedbackBridge();
    bool start(std::string& error);
    bool stop(std::string& error);
    bool failed() const;
    std::string error() const;
    void handle(const dualsense::DualSenseFeedback& feedback);
    void updateTriggerPositions(std::uint8_t left, std::uint8_t right);
    bool diagnosticTrigger(const ForceTriggerCommand& command, std::string& error);
    bool diagnosticGrips(std::uint8_t left, std::uint8_t right, std::string& error);
    dualsense::AdaptiveTriggerBridgeStats triggerStats() const;
    dualsense::RumbleBridgeStats rumbleStats() const;
    std::uint64_t hapticFrames() const;
    void setLogHandler(std::function<void(std::string_view)> handler);
    void enableEvdevRumble(int fd);
private:
    flydigi::Apex5Device& device_;
    Options options_;
    std::unique_ptr<dualsense::AdaptiveTriggerBridge> triggers_;
    std::unique_ptr<dualsense::RumbleBridge> rumble_;
    std::unique_ptr<dualsense::Apex6HapticBridge> apex6_;
    std::unique_ptr<EvdevRumble> evdev_;
    bool stopped_ = false;
};

} // namespace asb::platform::linux_backend
