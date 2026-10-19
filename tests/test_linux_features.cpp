#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/ApexProfileRestoreGuard.h"
#include "platform/linux/DualSenseReports.h"
#include "platform/linux/RuntimeSupport.h"

#include <algorithm>
#include <cassert>
#include <deque>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {
using namespace asb::platform::linux_backend;
using namespace std::chrono_literals;

class ProfileTransport final : public asb::platform::HidTransport {
public:
    ProfileTransport() { info_.vendorId = 0x37d7; info_.productId = 0x2501; info_.inputReportLength = info_.outputReportLength = 32; }
    bool isOpen() const noexcept override { return true; }
    const asb::HidDeviceInfo& info() const noexcept override { return info_; }
    bool writeOutputReport(std::span<const std::uint8_t> report, std::string& error) override {
        std::lock_guard lock(mutex_);
        std::array<std::uint8_t, 32> reply{4, 0x5a, 0xa5}; reply[3] = report[3];
        if (report[3] == 1) { reply[6] = 128; reply[7] = 1; reply[12] = 6; ++identityRequests; }
        else if (report[3] == asb::flydigi::kCmdApplyProfile) {
            profile = report[5];
            if (loseApplyReply.exchange(false)) { error = "lost profile ACK"; return false; }
        } else if (report[3] == asb::flydigi::kCmdProfileStatus) reply[6] = profile;
        else { error = "Unexpected test command"; return false; }
        replies_.push_back(reply); return true;
    }
    asb::platform::HidReadStatus readInputReport(std::span<std::uint8_t> data, std::chrono::milliseconds,
                                               std::size_t& size, std::string&) override {
        std::lock_guard lock(mutex_); size = 0;
        if (replies_.empty()) return asb::platform::HidReadStatus::Timeout;
        std::copy(replies_.front().begin(), replies_.front().end(), data.begin());
        replies_.pop_front(); size = 32; return asb::platform::HidReadStatus::Data;
    }
    std::atomic_uint profile{0}, identityRequests{0};
    std::atomic_bool loseApplyReply{false};
private:
    asb::HidDeviceInfo info_;
    std::mutex mutex_;
    std::deque<std::array<std::uint8_t, 32>> replies_;
};

Options options(std::initializer_list<std::string_view> args) { return parseOptions({args.begin(), args.size()}); }
void bad(std::initializer_list<std::string_view> args) {
    bool rejected = false;
    try { (void)options(args); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
}

void optionTests() {
    assert(!options({}).verbose);
    assert(options({"--verbose"}).verbose);
    assert(options({"list", "--json", "--verbose"}).json);
    auto o = options({"bridge-triggers", "--trigger-strength", "75", "--vibration-strength", "150", "--apex-profile", "4",
                      "--touchpad-profile", "death-stranding-2", "--seconds", "3600", "--gyro-yaw", "50"});
    assert(o.command == "bridge" && o.strength == 75 && o.rumbleStrength == 150 && o.apexProfile == 3);
    assert(o.touchpadProfile == asb::dualsense::TouchpadGestureProfile::DeathStranding2 && o.timed && o.seconds == 3600 && o.gyroYaw == 50);
    assert(options({"clear"}).command == "reset");
    assert(options({"test-input", "--json"}).command == "input-status");
    assert(options({"--version"}).command == "version");
    assert(options({"test-rgb", "--color", "#00aAFF"}).color == (std::array<std::uint8_t, 3>{0, 170, 255}));
    assert(options({"test-rgb"}).seconds == 2);
    auto motors = options({"test-rumble", "--left", "0", "--right", "255"});
    assert(motors.leftMotor == 0 && motors.rightMotor == 255);
    bad({"bridge", "--seconds", "86401"}); bad({"test-rgb", "--seconds", "31"});
    bad({"bridge", "--left", "50"}); bad({"test-rumble", "--left", "256"});
    bad({"bridge", "--apex-profile", "0"}); bad({"bridge", "--apex-profile", "5"});
    bad({"bridge", "--apex-profile", "1", "--apex-profile", "2"});
    bad({"bridge", "--touchpad-profile", "invalid"});
    bad({"bridge", "--touchpad-profile", "none", "--view-hold-swipe-up"});
    bad({"identify", "--touchpad-profile", "warframe"});
    bad({"test-trigger", "--mode", "unknown"}); bad({"test-trigger", "--side", "unknown"});
    bad({"test-trigger", "--level", "5"}); bad({"bridge", "--side", "lt"});
    bad({"test-rgb", "--color", "12345x"}); bad({"test-rgb", "--color", "ffffff00"});
    bad({"list", "--color", "ffffff"}); bad({"reset", "--json"});
    bad({"bridge", "--trace-hid", "same.json", "--telemetry-json", "./same.json"});
    bad({"bridge", "--gyro-yaw", "401"}); bad({"identify", "--telemetry-json", "x"});
    bad({"--device"}); bad({"bridge", "unknown"});
    for (const auto mode : {"resistance", "weapon", "vibration", "bow", "normal"}) {
        auto test = options({"test-trigger", "--mode", mode, "--level", "1", "--side", "lt"});
        auto effect = triggerTestCommand(test, asb::TriggerSide::Left);
        assert(effect.side == asb::TriggerSide::Left);
        if (std::string_view(mode) == "normal") assert(effect.mode == asb::TriggerMode::Normal);
        else { test.strength = 0; assert(triggerTestCommand(test, asb::TriggerSide::Right).mode == asb::TriggerMode::Normal); }
    }
}

void inputTests() {
    std::array<std::uint8_t, 32> identity{4, 0x5a, 0xa5, 1, 0, 0, 128, 1}; identity[12] = 6;
    auto initial = asb::flydigi::Apex5Identity::parseReply(identity); assert(initial);
    InputPipeline ds2(*initial, options({"bridge", "--touchpad-profile", "death-stranding-2", "--gyro-strength", "200", "--gyro-yaw", "50"}));
    std::array<std::uint8_t, 32> raw{4, 0x5a, 0xa5, 0xef};
    raw[12] = 0x40; raw[18] = 0x18; raw[19] = 0xfc; raw[20] = 0xe8; raw[21] = 3; // X=-1000,Y=1000
    auto physical = ds2.decode(raw); assert(physical && physical->gyroX == -2000 && physical->gyroY == 1000);
    const auto now = asb::dualsense::TouchpadGestureMapper::Clock::time_point{} + 1s;
    auto input = ds2.render(*physical, now); auto wire = buildUsbInput(input, 1, 0);
    assert(input.touch1Active && input.touch1X == 1600 && !(wire[33] & 0x80));
    assert(!physical->touch1Active); // Synthetic state never feeds back into physical controls.
    raw[12] = 0; raw[13] = 6; // LB + Menu -> left photo click, consume the chord.
    physical = ds2.decode(raw); input = ds2.render(*physical, now + 10ms);
    assert(input.touch1Active && input.touch1X == 320);
    assert(!(input.buttons & (asb::dualsense::button::kL1 | asb::dualsense::button::kOptions)));
    identity[12] = 0x13; // New battery/charge report is metadata, not a state sample.
    assert(!ds2.decode(identity));
    auto updated = asb::flydigi::Apex5Identity::parseReply(identity); assert(updated);
    input = ds2.render(*physical, now + 20ms);
    assert(input.batteryPercent == updated->batteryPercent() && input.chargeState == updated->chargeState());
    identity[6] = 84; identity[12] = 6; // Another model cannot overwrite this pad's metadata.
    assert(!ds2.decode(identity));
    assert(ds2.identity().batteryPercent() == updated->batteryPercent());
    InputPipeline swipe(*initial, options({"bridge", "--touchpad-profile", "miles-morales"}));
    raw[12] = 0x40; raw[13] = 0; physical = swipe.decode(raw);
    assert(!swipe.render(*physical, now).touch1Active);
    auto start = swipe.render(*physical, now + 450ms);
    auto middle = swipe.render(*physical, now + 540ms);
    assert(start.touch1Active && middle.touch1Active && middle.touch1X < start.touch1X);
    assert(swipe.gestureStats().swipes == 1);
    assert(!swipe.render(*physical, now + 700ms).touch1Active);
    asb::dualsense::DualSenseInputState extreme; extreme.gyroY = -32768;
    applyGyroStrength(extreme, 400, 400); assert(extreme.gyroY == -32768);
}

void profileAndBatteryTests() {
    auto* usb = new ProfileTransport();
    asb::flydigi::Apex5Device device{asb::flydigi::TransportPtr(usb)};
    std::string error; assert(device.verifyIdentity(error));
    {
        asb::ApexProfileRestoreGuard restore(device, 0);
        assert(selectProfile(device, 2, error) && usb->profile == 2);
        assert(restore.restore(error) && usb->profile == 0);
    }
    {
        asb::ApexProfileRestoreGuard restore(device, 0);
        usb->loseApplyReply = true;
        assert(!selectProfile(device, 3, error) && usb->profile == 3);
        assert(restore.restore(error) && usb->profile == 0); // Guard was armed before a lost ACK.
    }
    const auto requests = usb->identityRequests.load();
    BatteryRefresh battery(device, 5ms);
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (usb->identityRequests < requests + 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    battery.stop(); assert(usb->identityRequests >= requests + 2 && battery.failures() == 0);
    const auto stopped = usb->identityRequests.load();
    std::this_thread::sleep_for(15ms); assert(usb->identityRequests == stopped);
}

void statusTests() {
    SessionStatus status; status.inputs = 123; status.translations = 42;
    std::ostringstream console;
    printPeriodicStatus(console, status, false);
    assert(console.str().empty());
    printPeriodicStatus(console, status, true);
    assert(console.str().find("Input reports: 123") != std::string::npos);
    assert(console.str().find("trigger translations: 42") != std::string::npos);
    auto name = (std::filesystem::temp_directory_path() / "asb-features-XXXXXX").string();
    assert(::mkdtemp(name.data()));
    const auto root = std::filesystem::path(name);
    StatusFile file(root / "status.json"); file.status.phase = "running";
    file.status.device = "quoted\" path\n"; file.status.inputs = 123;
    file.write();
    auto read = [&] { std::ifstream f(root / "status.json"); return std::string(std::istreambuf_iterator<char>(f), {}); };
    auto first = read(); assert(first.find("quoted\\\" path\\n") != std::string::npos);
    file.status.phase = "stopped"; file.status.cleanupOk = true; file.write();
    assert(read().find("\"phase\":\"stopped\"") != std::string::npos);
    assert(read().find("\"cleanup_ok\":true") != std::string::npos);
    assert(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}) == 1);
    bool failed = false;
    try { StatusFile(root / "missing" / "x.json").write(); } catch (const std::runtime_error&) { failed = true; }
    assert(failed && std::filesystem::exists(root / "status.json"));
    std::filesystem::create_hard_link(root / "status.json", root / "alias.json");
    bad({"bridge", "--trace-hid", (root / "status.json").string(), "--telemetry-json", (root / "alias.json").string()});
    std::filesystem::remove_all(root);
}
}

int main() {
    optionTests(); inputTests(); profileAndBatteryTests(); statusTests();
    std::cout << "Linux options, touchpad, battery, profile restoration and telemetry tests passed\n";
}
