#pragma once

#include "platform/linux/InputStream.h"
#include "platform/linux/EvdevInput.h"
#include "platform/linux/RuntimeSupport.h"
#include "platform/linux/UhidDevice.h"

namespace asb::platform::linux_backend {

struct InputPumpStats {
    std::uint64_t reports = 0, motion = 0, forwarded = 0, heartbeats = 0;
};

// Single owner of physical input, the mapper and UHID. Fresh samples bypass
// heartbeat timing and are never coalesced: even adjacent down/up edges survive.
class InputPump {
public:
    using Clock = std::chrono::steady_clock;
    using Observer = std::function<void(const dualsense::DualSenseInputState&)>;
    InputPump(InputStream& input, InputPipeline& pipeline, UhidDevice& pad,
              const dualsense::DualSenseInputState& initial);
    InputPump(EvdevInput& input, InputPipeline& pipeline, UhidDevice& pad,
              const dualsense::DualSenseInputState& initial);
    bool step(const UhidDevice::Handler& feedback, const Observer& observer, std::string& error);
    InputPumpStats stats() const noexcept { return stats_; }
private:
    InputStream* input_ = nullptr;
    EvdevInput* evdev_ = nullptr;
    InputPipeline& pipeline_;
    UhidDevice& pad_;
    dualsense::DualSenseInputState physical_;
    Clock::time_point lastInput_ = Clock::now(), lastSent_ = lastInput_;
    InputPumpStats stats_;
};

} // namespace asb::platform::linux_backend
