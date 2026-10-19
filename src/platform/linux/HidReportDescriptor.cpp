#include "platform/linux/HidReportDescriptor.h"

#include <algorithm>
#include <array>
#include <vector>

namespace asb::platform::linux_backend {

std::optional<HidReportLayout> parseHidReportDescriptor(std::span<const std::uint8_t> data) {
    struct Globals { std::uint32_t page = 0, size = 0, count = 0, id = 0; } state;
    std::vector<Globals> stack;
    std::array<std::array<std::uint32_t, 256>, 3> bits{};
    HidReportLayout result;
    std::uint32_t usage = 0;
    bool hasUsage = false;
    unsigned depth = 0;
    for (std::size_t offset = 0; offset < data.size();) {
        const auto prefix = data[offset++];
        if (prefix == 0xfe) { // Long item: length, tag, payload.
            if (data.size() - offset < 2) return std::nullopt;
            const auto length = data[offset];
            offset += 2;
            if (data.size() - offset < length) return std::nullopt;
            offset += length;
            continue;
        }
        const auto length = (prefix & 3) == 3 ? 4U : prefix & 3U;
        if (data.size() - offset < length) return std::nullopt;
        std::uint32_t value = 0;
        for (unsigned i = 0; i < length; ++i) value |= std::uint32_t(data[offset++]) << (8 * i);
        const auto type = (prefix >> 2) & 3;
        const auto tag = prefix >> 4;
        if (type == 1) {
            switch (tag) {
            case 0: state.page = value; break;
            case 7: state.size = value; break;
            case 8:
                if (value == 0 || value > 255) return std::nullopt;
                state.id = value;
                result.numbered = true;
                break;
            case 9: state.count = value; break;
            case 10:
                if (stack.size() >= 32) return std::nullopt;
                stack.push_back(state);
                break;
            case 11:
                if (stack.empty()) return std::nullopt;
                state = stack.back(); stack.pop_back();
                break;
            }
        } else if (type == 2 && tag == 0) {
            usage = length == 4 ? value : (state.page << 16) | value;
        } else if (type == 0) {
            if (tag == 10) {
                if (depth++ == 0 && !hasUsage) {
                    result.usagePage = static_cast<std::uint16_t>(usage >> 16);
                    result.usage = static_cast<std::uint16_t>(usage);
                    hasUsage = true;
                }
            } else if (tag == 12) {
                if (depth == 0) return std::nullopt;
                --depth;
            } else if (tag == 8 || tag == 9 || tag == 11) {
                const unsigned kind = tag == 8 ? 0 : tag == 9 ? 1 : 2;
                const auto count = std::uint64_t(state.size) * state.count;
                auto& total = bits[kind][state.id];
                if (count + total > 32760) return std::nullopt;
                total += static_cast<std::uint32_t>(count);
            }
            usage = 0; // Local items expire at every main item.
        }
    }
    if (!hasUsage || depth != 0 || !stack.empty()) return std::nullopt;
    const auto maximum = [](const auto& lengths) {
        std::uint32_t maximum = 0;
        for (unsigned id = 0; id < 256; ++id) {
            if (lengths[id]) maximum = std::max(maximum, (lengths[id] + 7) / 8 + (id != 0));
        }
        return static_cast<std::uint16_t>(maximum);
    };
    result.inputLength = maximum(bits[0]);
    result.outputLength = maximum(bits[1]);
    result.featureLength = maximum(bits[2]);
    return result;
}

} // namespace asb::platform::linux_backend
