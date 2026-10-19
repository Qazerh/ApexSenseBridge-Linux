#pragma once

#include "dualsense/DualSenseInput.h"
#include "platform/HidTransport.h"

#include <array>
#include <bitset>
#include <linux/input.h>

namespace asb::platform::linux_backend {

struct EvdevLayout {
    std::array<input_absinfo, ABS_CNT> axes{};
    std::bitset<ABS_CNT> present;
    std::bitset<KEY_CNT> keys;
    bool hidTriggerOrder = false;
    bool rawHidButtons = false;
    unsigned buttonBase = BTN_GAMEPAD;
};

// Retains each SYN_REPORT boundary, including adjacent press/release frames.
class EvdevInput {
public:
    EvdevInput() = default;
    EvdevInput(int fd, EvdevLayout layout) : fd_(fd), layout_(std::move(layout)) {}
    ~EvdevInput();
    EvdevInput(const EvdevInput&) = delete;
    EvdevInput& operator=(const EvdevInput&) = delete;
    bool open(const HidDeviceInfo& device, bool grab, std::string& error);
    HidReadStatus read(std::optional<dualsense::DualSenseInputState>& state, std::string& error);
    dualsense::DualSenseInputState snapshot() const;
    int nativeHandle() const { return fd_; }
    bool buffered() const { return offset_ < count_; }
    const std::string& path() const { return path_; }
    bool hasRumble() const { return rumble_; }
private:
    bool resync(std::string& error);
    int fd_ = -1;
    EvdevLayout layout_;
    std::array<input_event, 64> events_{};
    std::size_t offset_ = 0, count_ = 0;
    std::string path_;
    bool lost_ = false, rumble_ = false;
};

} // namespace asb::platform::linux_backend
