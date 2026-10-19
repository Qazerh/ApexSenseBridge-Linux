#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace asb::platform::linux_backend {

struct HidReportLayout {
    std::uint16_t usagePage = 0;
    std::uint16_t usage = 0;
    std::uint16_t inputLength = 0;
    std::uint16_t outputLength = 0;
    std::uint16_t featureLength = 0;
    bool numbered = false;
    // hidraw output/feature APIs require an explicit zero ID for unnumbered HID.
    std::uint16_t outputBufferLength() const { return outputLength + (outputLength && !numbered); }
    std::uint16_t featureBufferLength() const { return featureLength + (featureLength && !numbered); }
};

// Maximum wire lengths, including a report ID only for numbered reports.
std::optional<HidReportLayout> parseHidReportDescriptor(std::span<const std::uint8_t> data);

} // namespace asb::platform::linux_backend
