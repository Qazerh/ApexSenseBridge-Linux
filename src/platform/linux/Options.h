#pragma once

#include "dualsense/TouchpadGestureProfile.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace asb::platform::linux_backend {

struct Options {
    std::string command = "bridge", device, traceFile, telemetryJson;
    unsigned strength = 100, seconds = 5, gyroStrength = 100, gyroYaw = 100, rumbleStrength = 100;
    unsigned leftMotor = 80, rightMotor = 80, level = 1;
    bool grab = true, lightbar = true, json = false, timed = false;
    bool lightbarRequested = false, verbose = false;
    std::string side = "both", mode = "resistance";
    std::optional<std::array<std::uint8_t, 3>> color;
    std::optional<std::uint8_t> apexProfile;
    dualsense::TouchpadGestureProfile touchpadProfile = dualsense::TouchpadGestureProfile::None;
};

Options parseOptions(std::span<const std::string_view> args);
Options parseOptions(int argc, char** argv);
void printHelp();

} // namespace asb::platform::linux_backend
