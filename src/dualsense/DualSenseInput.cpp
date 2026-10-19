#include "dualsense/DualSenseInput.h"

#include <algorithm>

namespace asb::dualsense {
std::uint8_t toBatteryPercent(std::uint8_t rawLevel) noexcept {
    if (rawLevel > 6) {
        return rawLevel > 100 ? static_cast<std::uint8_t>(100) : rawLevel;
    }
    switch (rawLevel) {
    case 6:
    case 5:
        return 100;
    case 4:
        return 80;
    case 3:
        return 60;
    case 2:
        return 40;
    case 1:
        return 20;
    case 0:
    default:
        return 10;
    }
}

std::uint8_t toChargeState(bool charging, bool wired, std::uint8_t batteryPercent) noexcept {
    if (charging || wired) {
        return batteryPercent >= 100
            ? chargeStatus::kFull
            : chargeStatus::kCharging;
    }
    return chargeStatus::kDischarging;
}

} // namespace asb::dualsense
