#include "core/ApexProfileRestoreGuard.h"
#include "dualsense/AdaptiveTriggerBridge.h"
#include "dualsense/LightbarBridge.h"
#include "dualsense/RumbleBridge.h"
#include "flydigi/Apex5Device.h"
#include "flydigi/Apex5Input.h"
#include "platform/linux/EvdevGrab.h"
#include "platform/linux/AsyncDiagnostics.h"
#include "platform/linux/InputPump.h"
#include "platform/linux/FeedbackBridge.h"
#include "platform/linux/InputStream.h"
#include "platform/linux/RuntimeSupport.h"
#include "platform/linux/UhidDevice.h"

#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
namespace linux_backend = asb::platform::linux_backend;
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }

using linux_backend::Options;

void require(bool ok, const std::string& error) {
    if (!ok) throw std::runtime_error(error.empty() ? "Operation failed" : error);
}

// Restore settings after normal stop, startup failure, signal or exception.
// No settings are saved to flash. SIGKILL/power loss cannot run this cleanup.
class SessionRestore {
public:
    SessionRestore(asb::flydigi::Apex5Device& device, std::uint8_t slot,
                   asb::flydigi::InputTransportStatus transport, linux_backend::SessionStatus* status)
        : device_(device), profile_(device, slot), transport_(transport), status_(status), originalSlot_(slot) {
        if (!device.identity()->isApex5()) profile_.dismiss();
    }
    ~SessionRestore() { (void)restore(); }
    void enableLightbar(std::uint8_t slot) {
        lightbar_ = std::make_unique<asb::dualsense::LightbarBridge>(device_, slot,
            asb::dualsense::LightbarBridgeOptions{20ms, true});
        require(!lightbar_->failed(), "Could not initialize RGB: " + lightbar_->error());
    }
    asb::dualsense::LightbarBridge* lightbar() const { return lightbar_.get(); }
    linux_backend::FeedbackBridge& enableFeedback(const Options& options) {
        feedback_ = std::make_unique<linux_backend::FeedbackBridge>(device_, options);
        return *feedback_;
    }
    void enableBatteryRefresh() { battery_ = std::make_unique<linux_backend::BatteryRefresh>(device_); }
    std::uint64_t batteryFailures() const { return battery_ ? battery_->failures() : 0; }
    bool restore() noexcept {
        if (done_) return succeeded_;
        done_ = true;
        try {
            if (battery_) battery_->stop();
            device_.stopAsyncWrites();
            std::string error;
            const auto attempt = [&](bool ok, const char* description) {
                if (!ok) {
                    std::cerr << description << ": " << error << '\n'; succeeded_ = false;
                    if (status_) status_->error += std::string(description) + ": " + error + "\n";
                }
                error.clear();
            };
            if (feedback_) attempt(feedback_->stop(error), "Could not stop controller feedback");
            else if (device_.identity()->supportsAdaptiveTriggers()) {
                attempt(device_.stopRumble(error), "Could not stop rumble");
                attempt(device_.clearAll(error), "Could not clear triggers");
            }
            // Stop/join the RGB worker before restoring profiles and routing.
            if (lightbar_) {
                lightbar_->restore();
                error = lightbar_->error();
                attempt(!lightbar_->failed(), "Could not complete RGB synchronization/restore lighting");
            }
            if (device_.identity()->isApex5()) {
                const bool profileOk = profile_.restore(error);
                attempt(profileOk, "Could not restore onboard profile");
                if (profileOk && status_) status_->activeProfile = originalSlot_ + 1;
                profile_.dismiss(); // Avoid a later profile application undoing transport restoration.
                // Profile effects/routing may be applied about one second after ACK.
                std::this_thread::sleep_for(1100ms);
                attempt(device_.setInputTransport(transport_.controllerData, transport_.rawData, error),
                        "Could not restore input routing; reconnect the controller");
            }
        } catch (...) {
            profile_.dismiss();
            succeeded_ = false;
            std::cerr << "Controller cleanup failed; reconnect the controller.\n";
            if (status_) status_->error += "Controller cleanup failed; reconnect the controller.\n";
        }
        if (status_) status_->cleanupOk = succeeded_;
        return succeeded_;
    }
private:
    asb::flydigi::Apex5Device& device_;
    asb::ApexProfileRestoreGuard profile_;
    asb::flydigi::InputTransportStatus transport_;
    std::unique_ptr<asb::dualsense::LightbarBridge> lightbar_;
    std::unique_ptr<linux_backend::BatteryRefresh> battery_;
    std::unique_ptr<linux_backend::FeedbackBridge> feedback_;
    linux_backend::SessionStatus* status_;
    std::uint8_t originalSlot_;
    bool done_ = false, succeeded_ = true;
};

std::optional<asb::dualsense::DualSenseInputState> readState(linux_backend::InputStream& stream,
                                                          linux_backend::InputPipeline& pipeline,
                                                          std::chrono::milliseconds timeout) {
    std::array<std::uint8_t, 4096> buffer{};
    std::size_t size = 0;
    std::string error;
    const auto status = stream.read(buffer, timeout, size, error);
    require(status != asb::platform::HidReadStatus::Error, error);
    if (status != asb::platform::HidReadStatus::Data) return std::nullopt;
    return pipeline.decode({buffer.data(), size});
}

std::optional<asb::dualsense::DualSenseInputState> initialState(linux_backend::InputStream& stream,
                                                           linux_backend::InputPipeline& pipeline) {
    const auto deadline = Clock::now() + 1500ms;
    unsigned reports = 0;
    while (!stopping && Clock::now() < deadline) {
        if (auto state = readState(stream, pipeline, 20ms); state && ++reports >= 3) return state;
    }
    return std::nullopt;
}

// Stable, locally administered synthetic address, unique for each USB location.
std::array<std::uint8_t, 6> addressFor(const asb::HidDeviceInfo& info) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto c : info.containerId + info.serial) { hash ^= static_cast<unsigned>(c); hash *= 1099511628211ULL; }
    std::array<std::uint8_t, 6> address{};
    for (unsigned i = 0; i < 5; ++i) address[i] = static_cast<std::uint8_t>(hash >> (8 * i));
    address[5] = 0x02;
    return address;
}

int runSession(const Options& options, linux_backend::StatusFile& telemetry) {
    if (options.command == "help") { linux_backend::printHelp(); return 0; }
    if (options.command == "version") { std::cout << "ApexSenseBridge " ASB_RELEASE_LABEL " Linux v9\n"; return 0; }
    std::ofstream traceFile;
    const auto traceStart = Clock::now();
    if (!options.traceFile.empty()) {
        traceFile.open(options.traceFile, std::ios::out | std::ios::trunc);
        require(traceFile.is_open(), "Cannot open HID trace file: " + options.traceFile);
        traceFile << "ApexSenseBridge Linux HID trace v8 (immediate ordered input, async diagnostics; touchpad profiles, battery, telemetry; independent input/fast RGB/rumble/setup 0x08)\n";
        traceFile.flush();
        require(bool(traceFile), "Cannot write HID trace: " + options.traceFile);
    }
    // Declared before device/restore: exception unwinding restores hardware
    // before joining a diagnostics worker that might be waiting on slow I/O.
    linux_backend::AsyncDiagnostics diagnostics;
    std::string error;
    auto candidates = asb::flydigi::Apex5Device::findCandidates(error);
    require(error.empty(), error);
    if (options.command == "list") {
        if (options.json) { std::cout << linux_backend::candidatesJson(candidates) << '\n'; return 0; }
        for (const auto& info : candidates) {
            std::cout << std::filesystem::path(info.path).string() << "  VID:PID "
                      << std::hex << std::setfill('0') << std::setw(4) << info.vendorId << ':'
                      << std::setw(4) << info.productId << std::dec
                      << "  input=" << info.inputReportLength << " output=" << info.outputReportLength << '\n';
        }
        if (candidates.empty()) std::cout << "No Apex 4/5/6 vendor interfaces found. Connect USB/2.4 GHz and wake the controller.\n";
        else std::cout << "Candidates only; the controller model is verified before any trigger writes.\n";
        return 0;
    }
    if (options.command == "diagnose") {
        const bool uhid = ::access("/dev/uhid", R_OK | W_OK) == 0;
        const bool driver = std::filesystem::exists("/sys/bus/hid/drivers/playstation");
        if (options.json)
            std::cout << "{\"uhid_read_write\":" << (uhid ? "true" : "false")
                      << ",\"hid_playstation_registered\":" << (driver ? "true" : "false")
                      << ",\"devices\":" << linux_backend::candidatesJson(candidates) << "}\n";
        else {
            std::cout << "UHID read/write access: " << (uhid ? "yes" : "no; check module and udev rules")
                      << "\nPlayStation HID driver registered: " << (driver ? "yes" : "no; check hid-playstation support")
                      << "\nApex candidates and permissions: " << linux_backend::candidatesJson(candidates) << '\n';
        }
        return 0;
    }
    if (!options.device.empty()) {
        std::erase_if(candidates, [&](const auto& info) { return std::filesystem::path(info.path) != options.device; });
    }
    require(!candidates.empty(), "No matching Apex vendor interface. Run 'ApexSenseBridge list'; use USB/2.4 GHz, not Bluetooth.");
    require(candidates.size() == 1, "More than one vendor interface found; select one with --device /dev/hidrawN.");
    auto device = asb::flydigi::Apex5Device::open(candidates.front(), error);
    require(device.has_value(), error);
    require(device->verifyIdentity(error, [] { return stopping != 0; }, true), error);
    const bool apex5 = device->identity()->isApex5();
    const bool apex6 = device->identity()->isApex6();
    const bool realtime = device->identity()->supportsRealtimeHaptics();
    if (!options.json) std::cout << device->identity()->describe() << " via " << device->identity()->connectionName() << std::endl;
    telemetry.status.device = std::filesystem::path(device->info().path).string();
    telemetry.status.connection = device->identity()->connectionName();
    telemetry.status.battery = device->identity()->batteryPercent();
    telemetry.status.chargeState = device->identity()->chargeState();
    telemetry.status.model = device->identity()->describe();
    telemetry.status.feedbackBackend = realtime ? "apex6-vcm" : apex6 ? "kernel-ff-rumble" : "forceadapt";
    telemetry.status.batteryAvailable = device->identity()->hasBatteryLevel();
    telemetry.status.motionSupported = !apex6;
    telemetry.status.rgbSupported = apex5;
    if (options.command == "identify") {
        if (options.json) std::cout << linux_backend::identityJson(device->info(), *device->identity()) << '\n';
        else if (device->identity()->hasBatteryLevel()) std::cout << "Battery: " << unsigned(device->identity()->batteryPercent()) << "%"
                       << (device->identity()->isCharging() ? " (charging)" : "") << '\n';
        else std::cout << "Battery level unavailable for this model.\n";
        return 0;
    }
    if (stopping) return 0;
    require(apex5 || (!options.apexProfile && options.command != "test-profile-switch"), "Onboard profile selection is supported on Apex 5 only");
    require(apex5 || options.command != "test-rgb", "RGB synchronization is supported on Apex 5 only");
    require(apex5 || !options.lightbarRequested, "RGB synchronization is supported on Apex 5 only");
    require(!apex6 || options.command != "test-gyro", "Apex 6 motion decoding is unavailable; use input-status for controls");
    require(!apex6 || realtime || options.command != "test-triggers", "Standard Apex 6 has no supported adaptive-trigger hardware; Pro trigger haptics require a verified Pro identity");
    if (options.command == "reset") {
        if (realtime) require(device->disableApex6Haptics(error), error);
        else if (!apex6) {
            require(device->clearAll(error), error);
            require(device->stopRumble(error), error);
        }
        std::cout << (apex6 && !realtime ? "Standard Apex 6 rumble is owned and released by the bridge's kernel client.\n" : "Both trigger effects and grip rumble stopped.\n");
        return 0;
    }
    linux_backend::EvdevInput mappedInput;
    if (apex6) require(mappedInput.open(device->info(), options.command == "bridge" && options.grab, error), error);
    asb::flydigi::ProfileStatus profile{};
    asb::flydigi::InputTransportStatus transport{};
    if (apex5) {
        require(device->readProfileStatus(profile, error), error);
        require(!profile.switchBank, "Switch profile bank is unsupported; select PC/XInput or DInput mode.");
        require(device->readInputTransportStatus(transport, error), error);
    }
    SessionRestore restore(*device, profile.slot, transport, &telemetry.status);
    telemetry.status.originalProfile = apex5 ? profile.slot + 1 : 0;
    telemetry.status.activeProfile = apex5 ? profile.slot + 1 : 0;
    auto activeSlot = options.apexProfile.value_or(profile.slot);
    if (options.command == "test-profile-switch" && !options.apexProfile) activeSlot = (profile.slot + 1) % 4;
    if (activeSlot != profile.slot) {
        require(linux_backend::selectProfile(*device, activeSlot, error), error);
        telemetry.status.activeProfile = activeSlot + 1;
    }
    if (options.command == "test-profile-switch") {
        std::cout << "Temporary onboard profile: " << unsigned(activeSlot + 1) << "; original: " << unsigned(profile.slot + 1) << '\n' << std::flush;
        const auto deadline = Clock::now() + std::chrono::seconds(options.seconds);
        while (!stopping && Clock::now() < deadline) std::this_thread::sleep_for(20ms);
        return restore.restore() ? 0 : 1;
    }
    if (stopping) return restore.restore() ? 0 : 1;
    if (!apex6) {
        require(device->clearAll(error), error);
        require(device->stopRumble(error), error);
    }
    auto& feedbackBridge = restore.enableFeedback(options);
    if (apex6 && !realtime && mappedInput.hasRumble()) feedbackBridge.enableEvdevRumble(mappedInput.nativeHandle());
    if (apex6 && !realtime && !mappedInput.hasRumble()) {
        telemetry.status.feedbackBackend = "unavailable";
        if (!options.json) std::cout << "Kernel FF_RUMBLE is unavailable; forwarding controls without feedback.\n";
    }
    feedbackBridge.setLogHandler([&diagnostics, verbose = options.verbose](std::string_view line) {
        if (!verbose) return;
        diagnostics.submit([line = std::string(line)] { std::cerr << line << '\n'; });
    });
    if (options.command == "test-rumble") {
        require(feedbackBridge.diagnosticGrips(options.leftMotor, options.rightMotor, error), error);
        if (realtime) require(feedbackBridge.start(error), error);
        std::cout << "Mild grip rumble for " << options.seconds << " seconds. Ctrl+C stops early.\n" << std::flush;
        const auto deadline = Clock::now() + std::chrono::seconds(options.seconds);
        while (!stopping && Clock::now() < deadline && !feedbackBridge.failed()) std::this_thread::sleep_for(20ms);
        if (feedbackBridge.failed()) require(false, feedbackBridge.error());
        return restore.restore() ? 0 : 1;
    }
    if (options.command == "test-triggers") {
        auto testOptions = options;
        testOptions.strength = 100; // The selected feedback backend applies gain once.
        for (const auto side : {asb::TriggerSide::Left, asb::TriggerSide::Right}) {
            if ((options.side == "lt" && side == asb::TriggerSide::Right) ||
                (options.side == "rt" && side == asb::TriggerSide::Left)) continue;
            require(feedbackBridge.diagnosticTrigger(linux_backend::triggerTestCommand(testOptions, side), error), error);
        }
        if (realtime) require(feedbackBridge.start(error), error);
        std::cout << options.mode << " on " << options.side << ", level " << options.level << " for " << options.seconds << " seconds. Ctrl+C stops early.\n" << std::flush;
        const auto deadline = Clock::now() + std::chrono::seconds(options.seconds);
        while (!stopping && Clock::now() < deadline && !feedbackBridge.failed()) std::this_thread::sleep_for(20ms);
        if (feedbackBridge.failed()) require(false, feedbackBridge.error());
        return restore.restore() ? 0 : 1;
    }

    if (apex5 && ((options.command == "bridge" && options.lightbar) || options.command == "test-rgb"))
        restore.enableLightbar(activeSlot);
    auto* lightbar = restore.lightbar();
    if (options.command == "test-rgb") {
        std::vector<std::array<std::uint8_t, 3>> colors{{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
        if (options.color) colors = {*options.color};
        for (const auto& color : colors) {
            if (stopping) break;
            asb::dualsense::DualSenseFeedback feedback;
            feedback.hasLightbar = true;
            feedback.lightbarRed = color[0]; feedback.lightbarGreen = color[1]; feedback.lightbarBlue = color[2];
            const auto previousWrites = lightbar->stats().writes;
            lightbar->handle(feedback);
            const auto deadline = Clock::now() + 3s;
            while (!stopping && !lightbar->failed() && lightbar->stats().writes == previousWrites && Clock::now() < deadline)
                std::this_thread::sleep_for(2ms);
            require(!lightbar->failed(), lightbar->error());
            if (stopping) break;
            require(lightbar->stats().writes > previousWrites, "RGB update timed out");
            std::cout << "RGB: " << unsigned(color[0]) << ',' << unsigned(color[1]) << ',' << unsigned(color[2]) << '\n' << std::flush;
            const auto until = Clock::now() + std::chrono::seconds(options.seconds);
            while (!stopping && Clock::now() < until) std::this_thread::sleep_for(20ms);
        }
        return restore.restore() ? 0 : 1;
    }

    linux_backend::InputStream inputStream;
    linux_backend::InputPipeline pipeline(*device->identity(), options);
    std::optional<asb::dualsense::DualSenseInputState> input;
    if (apex6) input = mappedInput.snapshot();
    else {
        require(inputStream.open(device->info(), error), error);
        if (apex5) require(device->setInputTransport(true, true, error), error);
        input = initialState(inputStream, pipeline);
    }
    if (apex5 && !input && !stopping) {
        // Firmware can acknowledge raw input while not emitting it; restart once.
        require(device->setInputTransport(true, false, error), error);
        require(device->setInputTransport(true, true, error), error);
        input = initialState(inputStream, pipeline);
    }
    if (stopping) return restore.restore() ? 0 : 1;
    require(input.has_value(), "Apex raw input did not start. For Apex 4 use DInput; wake/reconnect the controller and close other controller tools.");
    if (apex5) restore.enableBatteryRefresh();

    if (options.command == "test-gyro" || options.command == "input-status") {
        if (!options.json) std::cout << "Move the controls and rotate/tilt the controller. Gyro sensitivity: " << options.gyroStrength << "%.\n";
        const auto deadline = Clock::now() + std::chrono::seconds(options.seconds);
        auto lastInput = Clock::now(), lastPrint = Clock::now() - 200ms;
        std::uint64_t reports = 0, rotationReports = 0;
        while (!stopping && Clock::now() < deadline) {
            std::optional<asb::dualsense::DualSenseInputState> state;
            if (apex6) {
                pollfd ready{mappedInput.nativeHandle(), POLLIN, 0};
                const int result = mappedInput.buffered() ? 1 : ::poll(&ready, 1, 20);
                require(result >= 0 || errno == EINTR, "Cannot wait for gamepad input");
                if (result > 0) require(mappedInput.read(state, error) != asb::platform::HidReadStatus::Error, error);
            } else state = readState(inputStream, pipeline, 20ms);
            if (state) {
                input = state;
                lastInput = Clock::now();
                ++reports;
                if (state->gyroX || state->gyroY || state->gyroZ) ++rotationReports;
            }
            require(apex6 || Clock::now() - lastInput < 1500ms, "Apex motion input stopped");
            if (Clock::now() - lastPrint >= 200ms) {
                if (options.json) std::cout << linux_backend::inputJson(*input) << '\n' << std::flush;
                else {
                    if (options.command == "input-status") std::cout << "Buttons: " << input->buttons << " dpad: " << unsigned(input->dpad)
                        << " sticks: " << unsigned(input->lx) << ',' << unsigned(input->ly) << ',' << unsigned(input->rx) << ',' << unsigned(input->ry)
                        << " LT/RT: " << unsigned(input->l2) << '/' << unsigned(input->r2) << " battery: " << unsigned(input->batteryPercent) << "%  ";
                    std::cout << "Gyro: " << input->gyroX << ',' << input->gyroY << ',' << input->gyroZ
                              << "  Accel: " << input->accelX << ',' << input->accelY << ',' << input->accelZ << '\n' << std::flush;
                }
                lastPrint = Clock::now();
            }
        }
        if (!options.json) std::cout << "Sensor reports: " << reports << "; reports with rotation: " << rotationReports << '\n';
        if (!options.json && !stopping && !rotationReports)
            std::cout << "No rotation observed. Try again while rotating the controller; check that gyro strength is above zero.\n";
        return restore.restore() ? 0 : 1;
    }

    linux_backend::EvdevGrab grab;
    if (options.grab) require(grab.acquire(device->info(), error, apex6 ? mappedInput.path() : ""), error);
    else std::cerr << "Exclusive physical input disabled; duplicate game input is possible.\n";
    // No feedback callback may wait behind a whole RGB transaction. Wireless
    // pacing is retained; wired writes also run off the input/event thread.
    feedbackBridge.updateTriggerPositions(input->l2, input->r2);
    require(feedbackBridge.start(error), error);
    linux_backend::UhidDevice virtualPad;
    if (traceFile.is_open()) {
        virtualPad.setTraceHandler([&](std::string_view line) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - traceStart).count();
            diagnostics.submit([&traceFile, elapsed, line = std::string(line)] {
                traceFile << elapsed << "ms " << line << '\n';
                traceFile.flush();
                require(bool(traceFile), "Cannot write HID trace");
            });
        });
    }
    require(virtualPad.create(addressFor(device->info()), error), error);
    const auto startupDeadline = Clock::now() + 5s;
    const auto feedback = [&](const asb::dualsense::DualSenseFeedback& data) {
        feedbackBridge.handle(data);
        if (lightbar) lightbar->handle(data);
    };
    while (!stopping && !virtualPad.started() && Clock::now() < startupDeadline) {
        require(virtualPad.dispatch(feedback, error), error);
        std::this_thread::sleep_for(1ms);
    }
    if (stopping) { virtualPad.close(); return restore.restore() ? 0 : 1; }
    require(virtualPad.started(), "The kernel did not start the virtual DualSense. Check UHID/hid-playstation support.");
    require(virtualPad.update(pipeline.render(*input, Clock::now()), error), error);
    std::cout << "Bridge running. Launch your game with Steam Input disabled. Ctrl+C stops.\n" << std::flush;
    if (options.verbose) {
        std::cout << "Gyro sensitivity: " << options.gyroStrength << "%; RGB synchronization: "
                  << (lightbar ? "enabled" : "disabled") << "; rumble strength: " << options.rumbleStrength << "%\n" << std::flush;
        std::cout << "Touchpad profile: " << asb::dualsense::touchpadGestureProfileName(options.touchpadProfile)
                  << "; onboard profile: " << (apex5 ? std::to_string(activeSlot + 1) : "unmodified") << '\n' << std::flush;
        std::cout << "Feedback: " << telemetry.status.feedbackBackend << "; motion: " << (apex6 ? "unavailable" : "enabled") << "; RGB: " << (apex5 ? "available" : "unavailable on this model") << "\n" << std::flush;
    }
    if (traceFile.is_open()) std::cout << "HID trace: " << options.traceFile << '\n' << std::flush;
    auto lastStatus = Clock::now();
    auto pumpStorage = apex6
        ? std::make_unique<linux_backend::InputPump>(mappedInput, pipeline, virtualPad, *input)
        : std::make_unique<linux_backend::InputPump>(inputStream, pipeline, virtualPad, *input);
    auto& pump = *pumpStorage;
    const auto deadline = options.timed ? Clock::now() + std::chrono::seconds(options.seconds) : Clock::time_point::max();
    const auto sampleStatus = [&] {
        auto& s = telemetry.status;
        const auto triggersStats = feedbackBridge.triggerStats();
        const auto rgb = lightbar ? lightbar->stats() : asb::dualsense::LightbarBridgeStats{};
        const auto vibration = feedbackBridge.rumbleStats();
        const auto writes = device->asyncWriteStats();
        const auto touch = pipeline.gestureStats();
        const auto inputStats = pump.stats();
        s.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - traceStart).count();
        s.touchpadProfile = asb::dualsense::touchpadGestureProfileName(options.touchpadProfile);
        s.battery = pipeline.identity().batteryPercent(); s.chargeState = pipeline.identity().chargeState();
        s.inputs = inputStats.reports; s.outputs = virtualPad.outputs(); s.translations = triggersStats.translated;
        s.unsupported = triggersStats.unsupported; s.motion = inputStats.motion;
        s.inputForwarded = inputStats.forwarded; s.inputHeartbeats = inputStats.heartbeats;
        s.diagnosticDrops = diagnostics.dropped();
        s.hapticFrames = feedbackBridge.hapticFrames();
        s.rgbUpdates = rgb.updates; s.rgbWrites = rgb.writes; s.rgbErrors = rgb.writeFailures;
        s.rumbleUpdates = vibration.updates; s.rumbleWrites = vibration.writes;
        s.usbWrites = writes.writes + s.hapticFrames; s.coalesced = writes.coalesced; s.retries = device->asyncWriteRetries();
        s.touchTaps = touch.replayedTaps; s.touchSwipes = touch.swipes; s.batteryRefreshFailures = restore.batteryFailures();
    };
    const auto observeInput = [&](const asb::dualsense::DualSenseInputState& state) {
        feedbackBridge.updateTriggerPositions(state.l2, state.r2);
    };
    telemetry.status.phase = "running";
    sampleStatus();
    diagnostics.submit([snapshot = telemetry] { snapshot.write(); });
    while (!stopping && Clock::now() < deadline) {
        require(pump.step(feedback, observeInput, error), error);
        // Avoid taking error-string mutexes on every healthy input sample.
        if (feedbackBridge.failed()) require(false, feedbackBridge.error());
        require(!device->takeAsyncWriteError(error), error);
        if (lightbar && lightbar->failed()) require(false, lightbar->error());
        if (diagnostics.failed()) require(false, diagnostics.error());
        const auto now = Clock::now();
        if ((options.verbose || !options.telemetryJson.empty() || traceFile.is_open()) && now - lastStatus >= 5s) {
            const auto stats = feedbackBridge.triggerStats();
            const auto rgb = lightbar ? lightbar->stats() : asb::dualsense::LightbarBridgeStats{};
            const auto vibration = feedbackBridge.rumbleStats();
            sampleStatus();
            // Capture values, never references to the mapper/device. Only the
            // worker formats output and performs filesystem/terminal writes.
            diagnostics.submit([snapshot = telemetry, stats, rgb, vibration, &traceFile, verbose = options.verbose] {
                const auto& s = snapshot.status;
                snapshot.write();
                linux_backend::printPeriodicStatus(std::cout, s, verbose);
                if (traceFile.is_open()) {
                    traceFile << "STATUS inputs=" << s.inputs << " outputs=" << s.outputs
                              << " translations=" << stats.translated << " active=" << stats.active
                              << " unsupported=" << stats.unsupported << " motion=" << s.motion
                              << " rgb_updates=" << rgb.updates << " rgb_writes=" << rgb.writes
                              << " rgb_errors=" << rgb.writeFailures << " rumble_updates=" << vibration.updates
                              << " rumble_writes=" << vibration.writes << " battery=" << s.battery
                              << " touch_swipes=" << s.touchSwipes
                              << " battery_refresh_errors=" << s.batteryRefreshFailures
                              << " input_forwarded=" << s.inputForwarded << " input_heartbeats=" << s.inputHeartbeats
                              << " diagnostic_drops=" << s.diagnosticDrops
                              << " backend=" << s.feedbackBackend << " haptic_frames=" << s.hapticFrames
                              << " battery_available=" << s.batteryAvailable << '\n';
                    traceFile.flush();
                    require(bool(traceFile), "Cannot write HID trace");
                }
            });
            lastStatus = now;
        }
    }
    virtualPad.close();
    const bool restored = restore.restore();
    sampleStatus();
    diagnostics.finish();
    if (diagnostics.failed()) require(false, diagnostics.error());
    if (traceFile.is_open()) traceFile << "STOP cleanup=" << (restored ? "ok" : "error")
        << " input_forwarded=" << telemetry.status.inputForwarded
        << " diagnostic_drops=" << diagnostics.dropped() << '\n';
    if (restored) std::cout << "Stopped. Original controller settings restored.\n";
    if (options.verbose && diagnostics.dropped()) std::cout << diagnostics.dropped()
        << " diagnostic records skipped to keep input responsive.\n";
    return restored ? 0 : 1;
}

int run(const Options& options) {
    linux_backend::StatusFile telemetry(options.telemetryJson);
    telemetry.write(); // Validate the destination before touching controller settings.
    try {
        const int result = runSession(options, telemetry);
        telemetry.status.phase = result == 0 ? "stopped" : "error";
        if (result == 0 && telemetry.status.originalProfile == 0) telemetry.status.cleanupOk = true;
        telemetry.write();
        return result;
    } catch (const std::exception& error) {
        telemetry.status.phase = "error";
        telemetry.status.error = std::string(error.what()) + (telemetry.status.error.empty() ? "" : "\n" + telemetry.status.error);
        try { telemetry.write(); }
        catch (const std::exception& statusError) { std::cerr << "Telemetry: " << statusError.what() << '\n'; }
        throw;
    }
}
} // namespace

int main(int argc, char** argv) {
    struct sigaction action{};
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    // A closed output pipe must restore settings as well (e.g. --json | head).
    for (int signal : {SIGINT, SIGTERM, SIGHUP, SIGPIPE}) sigaction(signal, &action, nullptr);
    try { return run(linux_backend::parseOptions(argc, argv)); }
    catch (const std::exception& error) { std::cerr << "Error: " << error.what() << '\n'; return 1; }
}
