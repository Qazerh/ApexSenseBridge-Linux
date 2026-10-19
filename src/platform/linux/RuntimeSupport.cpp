#include "platform/linux/RuntimeSupport.h"

#include "cli/JsonSupport.h"
#include "dualsense/EffectStrength.h"
#include "flydigi/Apex5Input.h"
#include "flydigi/Apex4Input.h"
#include "platform/linux/DualSenseReports.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace asb::platform::linux_backend {
namespace {
std::string quote(std::string_view text) { return '"' + cli::jsonEscape(text) + '"'; }
}

std::optional<dualsense::DualSenseInputState> InputPipeline::decode(std::span<const std::uint8_t> packet) {
    if (const auto identity = identity_.isApex4() ? flydigi::Apex5Identity::parseApex4Reply(packet)
           : identity_.isApex6() ? flydigi::Apex5Identity::parseApex6Reply(packet, true)
                               : flydigi::Apex5Identity::parseReply(packet);
        identity && identity->deviceType() == identity_.deviceType() &&
        identity->connectionTypeRaw() == identity_.connectionTypeRaw()) identity_ = *identity;
    auto state = identity_.isApex4()
        ? flydigi::decodeApex4InputReport(packet, identity_.batteryPercent(), identity_.chargeState())
        : identity_.isApex5() ? flydigi::decodeApex5InputReport(packet, identity_.batteryPercent(), identity_.chargeState())
                            : std::nullopt;
    if (state) applyGyroStrength(*state, options_.gyroStrength, options_.gyroYaw);
    return state;
}

dualsense::DualSenseInputState InputPipeline::render(const dualsense::DualSenseInputState& physical,
                                                    dualsense::TouchpadGestureMapper::Clock::time_point now) {
    auto state = physical; // Never transform the previous synthetic touch sample again.
    state.batteryPercent = identity_.batteryPercent(); state.chargeState = identity_.chargeState();
    gestures_.transform(state, now);
    return state;
}

BatteryRefresh::BatteryRefresh(flydigi::Apex5Device& device, std::chrono::milliseconds interval) {
    thread_ = std::thread([this, &device, interval = std::max(interval, std::chrono::milliseconds(1))] {
        std::unique_lock lock(mutex_);
        while (!cv_.wait_for(lock, interval, [this] { return stopping_; })) {
            lock.unlock();
            std::string error;
            if (!device.requestBatteryRefresh(error)) ++failures_;
            lock.lock();
        }
    });
}
BatteryRefresh::~BatteryRefresh() { stop(); }
void BatteryRefresh::stop() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void printPeriodicStatus(std::ostream& out, const SessionStatus& s, bool verbose) {
    if (!verbose) return;
    out << "Input reports: " << s.inputs << "; output reports: " << s.outputs
        << "; trigger translations: " << s.translations << "; unsupported effects: " << s.unsupported
        << "; motion reports: " << s.motion << "; RGB updates/writes/errors: "
        << s.rgbUpdates << '/' << s.rgbWrites << '/' << s.rgbErrors
        << "; rumble updates/writes: " << s.rumbleUpdates << '/' << s.rumbleWrites
        << "; battery: " << (s.batteryAvailable ? std::to_string(s.battery) + "%" : "unavailable")
        << "; forwarded: " << s.inputForwarded << "; diagnostic drops: " << s.diagnosticDrops
        << '\n' << std::flush;
}

std::string statusJson(const SessionStatus& s) {
    std::ostringstream o;
    o << "{\"schema\":1,\"build\":\"linux-v9\",\"phase\":" << quote(s.phase)
      << ",\"error\":" << quote(s.error) << ",\"device\":" << quote(s.device)
      << ",\"connection\":" << quote(s.connection) << ",\"touchpad_profile\":" << quote(s.touchpadProfile)
      << ",\"cleanup_ok\":" << (s.cleanupOk ? "true" : "false")
      << ",\"original_profile\":" << s.originalProfile << ",\"active_profile\":" << s.activeProfile
      << ",\"battery_percent\":" << s.battery << ",\"charge_state\":" << s.chargeState
      << ",\"elapsed_ms\":" << s.elapsedMs << ",\"inputs\":" << s.inputs << ",\"outputs\":" << s.outputs
      << ",\"translations\":" << s.translations << ",\"unsupported\":" << s.unsupported
      << ",\"motion\":" << s.motion << ",\"rgb_updates\":" << s.rgbUpdates << ",\"rgb_writes\":" << s.rgbWrites
      << ",\"rgb_errors\":" << s.rgbErrors << ",\"rumble_updates\":" << s.rumbleUpdates << ",\"rumble_writes\":" << s.rumbleWrites
      << ",\"usb_writes\":" << s.usbWrites << ",\"coalesced\":" << s.coalesced << ",\"retries\":" << s.retries
      << ",\"touch_taps\":" << s.touchTaps << ",\"touch_swipes\":" << s.touchSwipes
      << ",\"battery_refresh_failures\":" << s.batteryRefreshFailures
      << ",\"input_forwarded\":" << s.inputForwarded << ",\"input_heartbeats\":" << s.inputHeartbeats
      << ",\"diagnostic_drops\":" << s.diagnosticDrops
      << ",\"model\":" << quote(s.model) << ",\"feedback_backend\":" << quote(s.feedbackBackend)
      << ",\"battery_available\":" << (s.batteryAvailable ? "true" : "false")
      << ",\"motion_supported\":" << (s.motionSupported ? "true" : "false")
      << ",\"rgb_supported\":" << (s.rgbSupported ? "true" : "false")
      << ",\"haptic_frames\":" << s.hapticFrames << '}';
    return o.str();
}

void StatusFile::write() const {
    if (path_.empty()) return;
    const auto data = statusJson(status) + '\n';
    auto name = path_.string() + ".tmp.XXXXXX";
    int fd = ::mkstemp(name.data());
    if (fd < 0) throw std::runtime_error("Cannot create telemetry snapshot: " + std::string(std::strerror(errno)));
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto count = ::write(fd, data.data() + offset, data.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { const auto reason = std::string(std::strerror(errno)); ::close(fd); ::unlink(name.c_str()); throw std::runtime_error("Cannot write telemetry: " + reason); }
        offset += static_cast<std::size_t>(count);
    }
    const bool closed = ::close(fd) == 0;
    if (!closed || ::rename(name.c_str(), path_.c_str()) < 0) {
        const auto reason = std::string(std::strerror(errno)); ::unlink(name.c_str());
        throw std::runtime_error("Cannot publish telemetry snapshot: " + reason);
    }
}

std::string identityJson(const HidDeviceInfo& info, const flydigi::Apex5Identity& identity) {
    std::ostringstream o;
    o << "{\"device\":" << quote(std::filesystem::path(info.path).string()) << ",\"model\":" << quote(identity.describe())
      << ",\"connection\":" << quote(identity.connectionName()) << ",\"device_type\":" << unsigned(identity.deviceType())
      << ",\"battery_percent\":" << unsigned(identity.batteryPercent()) << ",\"charging\":" << (identity.isCharging() ? "true" : "false")
      << ",\"battery_available\":" << (identity.hasBatteryLevel() ? "true" : "false")
      << ",\"forceadapt\":" << (identity.supportsAdaptiveTriggers() ? "true" : "false")
      << ",\"trigger_haptics\":" << (identity.supportsRealtimeHaptics() ? "true" : "false")
      << ",\"motion_supported\":" << (!identity.isApex6() ? "true" : "false")
      << ",\"rgb_supported\":" << (identity.isApex5() ? "true" : "false") << '}';
    return o.str();
}

std::string inputJson(const dualsense::DualSenseInputState& s) {
    std::ostringstream o;
    o << "{\"buttons\":" << s.buttons << ",\"dpad\":" << unsigned(s.dpad)
      << ",\"sticks\":[" << unsigned(s.lx) << ',' << unsigned(s.ly) << ',' << unsigned(s.rx) << ',' << unsigned(s.ry)
      << "],\"triggers\":[" << unsigned(s.l2) << ',' << unsigned(s.r2)
      << "],\"gyro\":[" << s.gyroX << ',' << s.gyroY << ',' << s.gyroZ
      << "],\"accel\":[" << s.accelX << ',' << s.accelY << ',' << s.accelZ
      << "],\"battery_percent\":" << unsigned(s.batteryPercent) << ",\"charge_state\":" << unsigned(s.chargeState) << '}';
    return o.str();
}

std::string candidatesJson(const std::vector<HidDeviceInfo>& devices) {
    std::ostringstream o; o << '[';
    for (std::size_t i = 0; i < devices.size(); ++i) {
        if (i) o << ',';
        const auto& d = devices[i];
        const auto path = std::filesystem::path(d.path).string();
        o << "{\"path\":" << quote(path) << ",\"vendor\":" << d.vendorId << ",\"product\":" << d.productId
          << ",\"input_bytes\":" << d.inputReportLength << ",\"output_bytes\":" << d.outputReportLength
          << ",\"readable\":" << (::access(path.c_str(), R_OK) == 0 ? "true" : "false")
          << ",\"writable\":" << (::access(path.c_str(), W_OK) == 0 ? "true" : "false") << '}';
    }
    o << ']'; return o.str();
}

ForceTriggerCommand triggerTestCommand(const Options& o, TriggerSide side) {
    ForceTriggerCommand command; command.side = side;
    const auto level = std::clamp(o.level, 1U, 4U) - 1;
    if (o.mode == "normal") return command;
    if (o.mode == "weapon") {
        command.mode = TriggerMode::SniperBreak;
        command.params = {std::array<std::uint8_t,4>{60,40,25,10}[level], std::array<std::uint8_t,4>{40,90,160,230}[level], std::array<std::uint8_t,4>{60,130,200,255}[level], 0, 0};
    } else if (o.mode == "vibration") {
        command.mode = TriggerMode::RecoilRattle;
        command.params = {std::array<std::uint8_t,4>{10,20,30,40}[level], 1, std::array<std::uint8_t,4>{30,60,100,150}[level], 0, 0};
    } else {
        command.mode = TriggerMode::Race;
        command.params = {std::array<std::uint8_t,4>{60,40,20,5}[level], std::array<std::uint8_t,4>{30,80,160,240}[level], 0, 0, 0};
        if (o.mode == "bow") command.params = {std::array<std::uint8_t,4>{40,25,15,5}[level], std::array<std::uint8_t,4>{40,90,160,230}[level], 0, 0, 0};
    }
    return dualsense::scaleTriggerStrength(command, o.strength);
}

bool selectProfile(flydigi::Apex5Device& device, std::uint8_t slot, std::string& error) {
    if (!device.applyProfile(slot, error)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    flydigi::ProfileStatus status;
    if (!device.readProfileStatus(status, error)) return false;
    if (status.switchBank || status.slot != slot) { error = "The controller did not retain the selected profile"; return false; }
    return true;
}

} // namespace asb::platform::linux_backend
