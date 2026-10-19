#include "platform/linux/Options.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace asb::platform::linux_backend {
namespace {
unsigned number(std::string_view text, unsigned minimum, unsigned maximum) {
    unsigned value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < minimum || value > maximum)
        throw std::runtime_error("Expected an integer in " + std::to_string(minimum) + ".." + std::to_string(maximum));
    return value;
}
bool oneOf(std::string_view value, std::initializer_list<std::string_view> choices) {
    return std::find(choices.begin(), choices.end(), value) != choices.end();
}
void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
}

Options parseOptions(std::span<const std::string_view> args) {
    Options o;
    bool commandSet = false, strengthSet = false, gyroSet = false, rumbleSet = false;
    bool triggerSet = false, motorsSet = false, touchSet = false, targetSet = false, lightbarSet = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto arg = args[i];
        if (arg == "--help" || arg == "-h") { o.command = "help"; return o; }
        if (arg == "--version") { o.command = "version"; return o; }
        if (arg == "--no-grab") { o.grab = false; continue; }
        if (arg == "--no-lightbar") { o.lightbar = false; o.lightbarRequested = false; lightbarSet = true; continue; }
        if (arg == "--sync-lightbar") { o.lightbar = true; o.lightbarRequested = true; lightbarSet = true; continue; }
        if (arg == "--once") { o.once = true; continue; }
        if (arg == "--verbose") { o.verbose = true; continue; }
        if (arg == "--json") { o.json = true; continue; }
        if (arg == "--view-hold-swipe-up") {
            check(!touchSet, "Choose only one touchpad profile"); touchSet = true;
            o.touchpadProfile = dualsense::TouchpadGestureProfile::LegacyViewHoldSwipeUp; continue;
        }
        if (arg == "--trigger-strength") arg = "--strength";
        if (arg == "--vibration-strength") arg = "--rumble-strength";
        if (oneOf(arg, {"--device", "--strength", "--seconds", "--trace-hid", "--telemetry-json",
                        "--gyro-strength", "--gyro-yaw", "--rumble-strength", "--touchpad-profile",
                        "--apex-profile", "--target", "--side", "--mode", "--level", "--left", "--right", "--color"})) {
            check(++i < args.size(), "Missing value for " + std::string(arg));
            const auto value = args[i];
            if (arg == "--device") o.device = value;
            else if (arg == "--trace-hid") o.traceFile = value;
            else if (arg == "--telemetry-json") o.telemetryJson = value;
            else if (arg == "--strength") { o.strength = number(value, 0, 100); strengthSet = true; }
            else if (arg == "--seconds") { o.seconds = number(value, 1, 86400); o.timed = true; }
            else if (arg == "--gyro-strength") { o.gyroStrength = number(value, 0, 400); gyroSet = true; }
            else if (arg == "--gyro-yaw") { o.gyroYaw = number(value, 0, 400); gyroSet = true; }
            else if (arg == "--rumble-strength") { o.rumbleStrength = number(value, 0, 200); rumbleSet = true; }
            else if (arg == "--left") { o.leftMotor = number(value, 0, 255); motorsSet = true; }
            else if (arg == "--right") { o.rightMotor = number(value, 0, 255); motorsSet = true; }
            else if (arg == "--level") { o.level = number(value, 1, 4); triggerSet = true; }
            else if (arg == "--side") { o.side = value; triggerSet = true; }
            else if (arg == "--mode") { o.mode = value; triggerSet = true; }
            else if (arg == "--apex-profile" || arg == "--target") {
                check(!o.apexProfile, "Choose only one onboard profile");
                o.apexProfile = static_cast<std::uint8_t>(number(value, 1, 4) - 1); targetSet = arg == "--target";
            } else if (arg == "--touchpad-profile") {
                check(!touchSet, "Choose only one touchpad profile"); touchSet = true;
                auto profile = dualsense::parseTouchpadGestureProfile(value);
                check(profile.has_value(), "Unknown touchpad profile; see --help"); o.touchpadProfile = *profile;
            } else if (arg == "--color") {
                auto hex = value;
                if (hex.starts_with('#')) hex.remove_prefix(1);
                unsigned rgb = 0;
                auto parsed = std::from_chars(hex.data(), hex.data() + hex.size(), rgb, 16);
                check(hex.size() == 6 && parsed.ec == std::errc{} && parsed.ptr == hex.data() + hex.size(), "Expected RGB color RRGGBB or #RRGGBB");
                o.color = {{static_cast<std::uint8_t>(rgb >> 16), static_cast<std::uint8_t>(rgb >> 8), static_cast<std::uint8_t>(rgb)}};
            }
        } else if (!commandSet && !arg.starts_with('-')) {
            commandSet = true;
            if (arg == "bridge-triggers") arg = "bridge";
            if (arg == "test-trigger") arg = "test-triggers";
            if (arg == "clear") arg = "reset";
            if (arg == "test-input") arg = "input-status";
            check(oneOf(arg, {"bridge", "list", "identify", "diagnose", "input-status", "test-triggers", "test-rumble", "test-gyro", "test-rgb", "test-profile-switch", "reset"}), "Unknown command: " + std::string(arg));
            o.command = arg;
        } else throw std::runtime_error("Unknown argument: " + std::string(arg));
    }
    const auto& c = o.command;
    if (o.timed) {
        check(oneOf(c, {"bridge", "input-status", "test-triggers", "test-rumble", "test-gyro", "test-rgb", "test-profile-switch"}), "--seconds requires bridge or a timed test");
        if (c != "bridge") check(o.seconds <= 30, "Test duration must be in 1..30 seconds");
    } else if (c == "test-rgb") o.seconds = 2;
    check(!o.once || c == "bridge", "--once requires bridge");
    check(!strengthSet || oneOf(c, {"bridge", "test-triggers"}), "--strength requires bridge or test-triggers");
    check(!gyroSet || oneOf(c, {"bridge", "test-gyro", "input-status"}), "Gyro tuning requires bridge, test-gyro or input-status");
    check(!rumbleSet || oneOf(c, {"bridge", "test-rumble"}), "--rumble-strength requires bridge or test-rumble");
    check(!triggerSet || c == "test-triggers", "--side/--mode/--level require test-triggers");
    check(oneOf(o.side, {"lt", "rt", "both"}), "--side must be lt, rt or both");
    check(oneOf(o.mode, {"resistance", "weapon", "vibration", "bow", "normal"}), "Unknown trigger test mode");
    check(!motorsSet || c == "test-rumble", "--left/--right require test-rumble");
    check(!o.color || c == "test-rgb", "--color requires test-rgb");
    check(!touchSet || c == "bridge", "Touchpad profiles require bridge");
    check(!o.apexProfile || oneOf(c, {"bridge", "test-profile-switch"}), "--apex-profile requires bridge or test-profile-switch");
    check(!targetSet || c == "test-profile-switch", "--target requires test-profile-switch");
    check(o.grab || c == "bridge", "--no-grab requires bridge");
    check(!lightbarSet || c == "bridge", "Lightbar options require bridge");
    check(o.traceFile.empty() || c == "bridge", "--trace-hid requires bridge");
    check(o.telemetryJson.empty() || c == "bridge", "--telemetry-json requires bridge");
    check(!o.json || oneOf(c, {"list", "diagnose", "identify", "input-status", "test-gyro"}), "--json requires list, diagnose, identify, input-status or test-gyro");
    if (!o.traceFile.empty() && !o.telemetryJson.empty()) {
        check(std::filesystem::absolute(o.traceFile).lexically_normal() != std::filesystem::absolute(o.telemetryJson).lexically_normal(), "Trace and telemetry paths must differ");
        std::error_code error;
        const bool sameFile = std::filesystem::equivalent(o.traceFile, o.telemetryJson, error);
        check(error || !sameFile, "Trace and telemetry must not refer to the same file");
    }
    return o;
}

Options parseOptions(int argc, char** argv) {
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return parseOptions(args);
}

void printHelp() {
    std::cout << "ApexSenseBridge " ASB_RELEASE_LABEL " Linux v10 — Apex 4 / 5 / 6 / 6 Pro\n\n"
        "Usage: ApexSenseBridge [command] [options]\n"
        "  bridge              Forward input, gyro, adaptive triggers, rumble and RGB (default)\n"
        "  list / diagnose     List candidates / check interfaces and permissions\n"
        "  identify            Verify model, connection and battery without changing settings\n"
        "  input-status        Display buttons, sticks, triggers and motion for 5 seconds\n"
        "  test-triggers       Apex 4/5 resistance or 6 Pro trigger haptics; lt/rt/both\n"
        "                      --mode resistance|weapon|vibration|bow|normal\n"
        "                      --side lt|rt|both --level 1..4 (default 1, mild)\n"
        "  test-rumble         Test --left 0..255 --right 0..255 (default 80 each)\n"
        "  test-gyro           Apex 4/5 rotation/acceleration for 5 seconds\n"
        "  test-rgb            Apex 5: cycle RGB or test --color RRGGBB\n"
        "  test-profile-switch Temporarily select --target 1..4, then restore\n"
        "  reset               Stop triggers and rumble\n\n"
        "  --once              Exit instead of waiting/reconnecting (bridge only)\n"
        "  --device PATH       Select /dev/hidrawN from list\n"
        "  --seconds N         Bridge limit 1..86400; tests 1..30 (RGB: per color)\n"
        "  --strength N        Trigger strength 0..100 (default 100)\n"
        "  --rumble-strength N Grip rumble 0..200 (default 100)\n"
        "  --gyro-strength N   Rotation sensitivity 0..400 (default 100)\n"
        "  --gyro-yaw N        Additional yaw sensitivity 0..400 (default 100)\n"
        "  --apex-profile N    Apex 5: temporary slot 1..4; restored on exit\n"
        "  --touchpad-profile NAME  none, legacy-view-hold-swipe-up, spider-man-2,\n"
        "                      miles-morales, ghost-of-tsushima, warframe, death-stranding-2\n"
        "  --no-lightbar       Keep original lighting\n"
        "  --no-grab           Skip physical gamepad isolation\n"
        "  --trace-hid FILE    Bounded HID trace\n"
        "  --telemetry-json FILE  Atomic status snapshots, including final errors\n"
        "  --verbose           Show live statistics and trigger diagnostics\n"
        "  --json              Machine-readable diagnostics\n"
        "  --version / --help  Build version / this help\n\n"
        "Aliases: bridge-triggers, test-trigger, test-input, clear,\n"
        "--trigger-strength, --vibration-strength, --view-hold-swipe-up.\n"
        "Use USB or the 2.4 GHz receiver. Keep Steam Input disabled for native feedback.\n"
        "Apex 4: DInput; Apex 6/Pro: PC/XInput. RGB/profiles: Apex 5 only.\n"
        "Bridge waits for a controller and reconnects automatically.\n"
        "Ctrl+C restores the controller. See docs/linux.md.\n";
}

} // namespace asb::platform::linux_backend
