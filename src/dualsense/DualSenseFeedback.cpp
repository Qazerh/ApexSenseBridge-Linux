#include "dualsense/DualSenseFeedback.h"

#include <algorithm>

namespace asb::dualsense {
bool DualSenseFeedback::hasRumble() const {
    return kind == FeedbackKind::HidOutput && (rumbleLeft != 0 || rumbleRight != 0);
}

bool DualSenseFeedback::requestsRumbleUpdate() const {
    constexpr std::uint8_t kCompatibleVibration = 0x01;
    constexpr std::uint8_t kCompatibleVibration2 = 0x04;
    return kind == FeedbackKind::HidOutput &&
           (((enableBits1 & kCompatibleVibration) != 0) ||
            ((enableBits3 & kCompatibleVibration2) != 0));
}

bool DualSenseFeedback::hasTriggerEffect() const {
    if (kind != FeedbackKind::HidOutput) {
        return false;
    }
    const auto nonZero = [](std::uint8_t value) { return value != 0; };
    return std::any_of(leftTriggerEffect.begin(), leftTriggerEffect.end(), nonZero) ||
           std::any_of(rightTriggerEffect.begin(), rightTriggerEffect.end(), nonZero);
}

bool DualSenseFeedback::hasLightbarColor() const noexcept {
    return kind == FeedbackKind::HidOutput && hasLightbar;
}

} // namespace asb::dualsense
