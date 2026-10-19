#include "platform/linux/FeedbackBridge.h"
#include "dualsense/EffectStrength.h"
#include <stdexcept>

namespace asb::platform::linux_backend {
using namespace std::chrono_literals;

FeedbackBridge::FeedbackBridge(flydigi::Apex5Device& device, const Options& options) : device_(device), options_(options) {
    if (!device.identity()) throw std::runtime_error("Controller feedback requires verified identity");
    if (device.identity()->supportsAdaptiveTriggers()) {
        triggers_ = std::make_unique<dualsense::AdaptiveTriggerBridge>(device, options.strength);
        rumble_ = std::make_unique<dualsense::RumbleBridge>(device, haptics::HapticConfig{}, options.rumbleStrength);
    } else if (device.identity()->supportsRealtimeHaptics()) {
        apex6_ = std::make_unique<dualsense::Apex6HapticBridge>(device, haptics::HapticConfig{}, true, options.strength, options.rumbleStrength);
    }
}
FeedbackBridge::~FeedbackBridge() { std::string ignored; (void)stop(ignored); }
bool FeedbackBridge::start(std::string& error) {
    stopped_ = false;
    if (apex6_) return apex6_->start(error);
    return !triggers_ || device_.startAsyncWrites(error, device_.asyncWritePolicy().value_or(flydigi::AsyncWritePolicy{1ms, 0ms}));
}
bool FeedbackBridge::stop(std::string& error) {
    if (stopped_) return true;
    stopped_ = true;
    if (apex6_) return apex6_->stop(error);
    if (evdev_) return evdev_->stop(error);
    device_.stopAsyncWrites();
    std::string triggerError, rumbleError;
    const bool triggerOk = !triggers_ || device_.clearAll(triggerError);
    const bool rumbleOk = !rumble_ || device_.stopRumble(rumbleError);
    error = triggerError + (triggerError.empty() || rumbleError.empty() ? "" : "; ") + rumbleError;
    return triggerOk && rumbleOk;
}
void FeedbackBridge::enableEvdevRumble(int fd) { evdev_ = std::make_unique<EvdevRumble>(fd, options_.rumbleStrength); }
bool FeedbackBridge::failed() const { return (triggers_ && triggers_->failed()) || (rumble_ && rumble_->failed()) || (apex6_ && apex6_->failed()) || (evdev_ && evdev_->failed()); }
std::string FeedbackBridge::error() const {
    if (triggers_ && triggers_->failed()) return triggers_->error();
    if (rumble_ && rumble_->failed()) return rumble_->error();
    if (apex6_ && apex6_->failed()) return apex6_->error();
    return evdev_ && evdev_->failed() ? evdev_->error() : "";
}
void FeedbackBridge::handle(const dualsense::DualSenseFeedback& feedback) {
    if (triggers_) triggers_->handle(feedback);
    if (rumble_) rumble_->handle(feedback);
    if (apex6_) apex6_->handle(feedback);
    if (evdev_) evdev_->handle(feedback);
}
void FeedbackBridge::updateTriggerPositions(std::uint8_t left, std::uint8_t right) {
    if (triggers_) triggers_->updateTriggerPositions(left, right);
    if (apex6_) apex6_->updateTriggerPositions(left, right);
}
void FeedbackBridge::setLogHandler(std::function<void(std::string_view)> handler) { if (triggers_) triggers_->setLogHandler(std::move(handler)); }
bool FeedbackBridge::diagnosticTrigger(const ForceTriggerCommand& command, std::string& error) {
    if (apex6_) { apex6_->setDiagnosticTrigger(command); apex6_->updateTriggerPositions(255, 255); return true; }
    if (triggers_) return device_.setTriggerRaw(dualsense::scaleTriggerStrength(command, options_.strength), error);
    error = "This model has no supported adaptive/trigger-haptic hardware"; return false;
}
bool FeedbackBridge::diagnosticGrips(std::uint8_t left, std::uint8_t right, std::string& error) {
    if (apex6_) { apex6_->setDiagnosticGrips(left, right); return true; }
    if (evdev_) { evdev_->set(left, right); return true; }
    if (rumble_) return device_.setRumble(dualsense::scaleRumbleStrength(left, options_.rumbleStrength),
                                         dualsense::scaleRumbleStrength(right, options_.rumbleStrength), error);
    error = "The kernel gamepad driver does not expose FF_RUMBLE for this model"; return false;
}
dualsense::AdaptiveTriggerBridgeStats FeedbackBridge::triggerStats() const {
    if (triggers_) return triggers_->stats();
    dualsense::AdaptiveTriggerBridgeStats stats;
    if (apex6_) { const auto a = apex6_->stats(); stats.translated = a.triggerLeftUpdates + a.triggerRightUpdates; stats.active = a.triggerActiveUpdates; stats.unsupported = a.triggerUnsupported + a.triggerMalformed; }
    return stats;
}
dualsense::RumbleBridgeStats FeedbackBridge::rumbleStats() const {
    if (rumble_) return rumble_->stats();
    dualsense::RumbleBridgeStats stats;
    if (apex6_) { const auto a = apex6_->stats(); stats.updates = a.rumbleUpdates; stats.writes = a.framesWritten; }
    if (evdev_) { stats.updates = evdev_->updates(); stats.writes = evdev_->writes(); }
    return stats;
}
std::uint64_t FeedbackBridge::hapticFrames() const { return apex6_ ? apex6_->stats().framesWritten : 0; }

} // namespace asb::platform::linux_backend
