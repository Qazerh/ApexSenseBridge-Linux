#pragma once

#include "dualsense/DualSenseFeedback.h"
#include "dualsense/DualSenseInput.h"

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace asb::platform::linux_backend {

std::span<const std::uint8_t> dualSenseDescriptor();
// Changes rotation sensitivity only; acceleration retains its physical units.
void applyGyroStrength(dualsense::DualSenseInputState& state, unsigned percent, unsigned yawPercent = 100);
std::array<std::uint8_t, 64> buildUsbInput(const dualsense::DualSenseInputState& state,
                                        std::uint8_t sequence, std::uint32_t timestamp);
std::optional<dualsense::DualSenseFeedback> decodeUsbOutput(std::span<const std::uint8_t> report);
std::vector<std::uint8_t> featureReport(std::uint8_t id, const std::array<std::uint8_t, 6>& address);

} // namespace asb::platform::linux_backend
