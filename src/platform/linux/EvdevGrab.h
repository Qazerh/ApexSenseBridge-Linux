#pragma once
#include "core/DeviceInfo.h"
#include <string>
#include <vector>

namespace asb::platform::linux_backend {
class EvdevGrab {
public:
    ~EvdevGrab();
    EvdevGrab() = default;
    EvdevGrab(const EvdevGrab&) = delete;
    EvdevGrab& operator=(const EvdevGrab&) = delete;
    bool acquire(const HidDeviceInfo& device, std::string& error, const std::string& alreadyGrabbed = {});
private:
    std::vector<int> fds_;
};
} // namespace asb::platform::linux_backend
