#include "platform/linux/BridgeLifecycle.h"
#include "platform/linux/RuntimeSupport.h"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <poll.h>

namespace asb::platform::linux_backend {

std::optional<HidDeviceInfo> ControllerSelector::select(const std::vector<HidDeviceInfo>& candidates) const {
    std::vector<HidDeviceInfo> matches;
    for (const auto& info : candidates) {
        bool match = true;
        if (device_) {
            match = info.vendorId == device_->vendorId && info.productId == device_->productId &&
                    info.interfaceNumber == device_->interfaceNumber;
            if (match) {
                if (!device_->serial.empty()) match = info.serial == device_->serial;
                else if (!device_->containerId.empty()) match = info.containerId == device_->containerId;
                else match = info.path == device_->path;
            }
        } else if (!path_.empty()) match = std::filesystem::path(info.path) == path_;
        if (match) matches.push_back(info);
    }
    if (matches.size() > 1) throw std::runtime_error("More than one matching controller; select one with --device /dev/hidrawN.");
    if (matches.empty()) return std::nullopt;
    return matches.front();
}

void ControllerSelector::remember(const HidDeviceInfo& device, std::uint8_t model) {
    if (model_ && *model_ != model)
        throw std::runtime_error("The receiver's controller model changed. Restart the bridge to select the new model.");
    device_ = device;
    model_ = model;
}

void FeedbackState::remember(const dualsense::DualSenseFeedback& f) {
    if (f.kind != dualsense::FeedbackKind::HidOutput) return;
    for (const auto bit : {4, 8}) {
        if (!(f.enableBits1 & bit)) continue;
        state_.enableBits1 |= bit;
        if (bit == 4) state_.rightTriggerEffect = f.rightTriggerEffect;
        else state_.leftTriggerEffect = f.leftTriggerEffect;
    }
    if (f.hasLightbarColor()) {
        state_.hasLightbar = true;
        state_.enableBits2 |= 4;
        state_.lightbarRed = f.lightbarRed;
        state_.lightbarGreen = f.lightbarGreen;
        state_.lightbarBlue = f.lightbarBlue;
    }
}

void IdlePad::start() {
    if (worker_.joinable() || pad_.nativeHandle() < 0 || failed()) return;
    stopping_ = false;
    worker_ = std::thread(&IdlePad::run, this);
}
void IdlePad::stop() {
    stopping_ = true;
    if (worker_.joinable()) worker_.join();
}
std::string IdlePad::error() const { std::lock_guard lock(mutex_); return error_; }
void IdlePad::run() noexcept {
    try {
        std::string error;
        const auto remember = [&](const dualsense::DualSenseFeedback& f) { feedback_.remember(f); };
        while (!stopping_.load(std::memory_order_relaxed)) {
            if (!pad_.update({}, error)) throw std::runtime_error(error);
            pollfd ready{pad_.nativeHandle(), POLLIN, 0};
            const int result = ::poll(&ready, 1, 8);
            if (result < 0 && errno == EINTR) continue;
            if (result < 0 || (ready.revents & (POLLERR | POLLHUP | POLLNVAL)))
                throw std::runtime_error("UHID connection closed while waiting for the controller");
            if ((ready.revents & POLLIN) && !pad_.dispatch(remember, error)) throw std::runtime_error(error);
            if (!pad_.started()) throw std::runtime_error("Virtual DualSense stopped unexpectedly");
        }
    } catch (const std::exception& e) {
        { std::lock_guard lock(mutex_); error_ = e.what(); }
        failed_.store(true, std::memory_order_release);
    }
}

bool DeferredSettings::recover(flydigi::Apex5Device& device, std::string& error) {
    if (!pending) return true;
    if (!device.identity() || !device.identity()->isApex5() || device.identity()->deviceType() != model) {
        error = "Cannot restore settings to a different controller model";
        return false;
    }
    if (!device.clearAll(error) || !device.stopRumble(error)) return false;
    if (rgb) {
        auto latched = *rgb;
        latched[2] = 1;
        if (!device.writeRgbConfig(rgbSlot, latched, error) ||
            !device.writeRgbConfigRange(rgbSlot, 0, 1, *rgb, error)) return false;
    }
    if (!selectProfile(device, originalSlot, error) ||
        !device.setInputTransport(transport.controllerData, transport.rawData, error)) return false;
    pending = false;
    rgb.reset();
    return true;
}

} // namespace asb::platform::linux_backend
