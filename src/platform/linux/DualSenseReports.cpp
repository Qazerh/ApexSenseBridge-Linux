#include "platform/linux/DualSenseReports.h"

#include <algorithm>

namespace asb::platform::linux_backend {
namespace {
template<class Buffer> void put16(Buffer& data, std::size_t offset, std::int16_t value) {
    const auto raw = static_cast<std::uint16_t>(value);
    data[offset] = static_cast<std::uint8_t>(raw);
    data[offset + 1] = static_cast<std::uint8_t>(raw >> 8);
}

std::uint8_t hat(std::uint8_t dpad) {
    // Internal state uses Up/Down/Left/Right bits, USB uses an eight-way hat.
    switch (dpad & 15) {
    case 1: return 0; case 9: return 1; case 8: return 2; case 10: return 3;
    case 2: return 4; case 6: return 5; case 4: return 6; case 5: return 7;
    default: return 8;
    }
}
} // namespace

void applyGyroStrength(dualsense::DualSenseInputState& state, unsigned percent, unsigned yawPercent) {
    const auto scale = [percent](std::int16_t value) {
        const auto scaled = static_cast<std::int32_t>(value) *
                            static_cast<std::int32_t>(std::min(percent, 400U)) / 100;
        return static_cast<std::int16_t>(std::clamp<std::int32_t>(scaled, -32768, 32767));
    };
    state.gyroX = scale(state.gyroX);
    const auto yaw = static_cast<std::int64_t>(state.gyroY) * std::min(percent, 400U) * std::min(yawPercent, 400U) / 10000;
    state.gyroY = static_cast<std::int16_t>(std::clamp<std::int64_t>(yaw, -32768, 32767));
    state.gyroZ = scale(state.gyroZ);
}

std::array<std::uint8_t, 64> buildUsbInput(const dualsense::DualSenseInputState& s,
                                         std::uint8_t sequence, std::uint32_t timestamp) {
    std::array<std::uint8_t, 64> report{};
    report[0] = 1;
    report[1] = s.lx; report[2] = s.ly; report[3] = s.rx; report[4] = s.ry;
    report[5] = s.l2; report[6] = s.r2; report[7] = sequence;
    report[8] = static_cast<std::uint8_t>((s.buttons & 0xf0) | hat(s.dpad));
    report[9] = static_cast<std::uint8_t>(s.buttons >> 8);
    report[10] = static_cast<std::uint8_t>(s.buttons & 7);
    put16(report, 16, s.gyroX); put16(report, 18, s.gyroY); put16(report, 20, s.gyroZ);
    put16(report, 22, s.accelX); put16(report, 24, s.accelY); put16(report, 26, s.accelZ);
    for (unsigned i = 0; i < 4; ++i) report[28 + i] = static_cast<std::uint8_t>(timestamp >> (8 * i));
    const auto touch = [&](std::size_t offset, bool active, unsigned id, unsigned x, unsigned y) {
        report[offset] = static_cast<std::uint8_t>(id | (active ? 0 : 0x80));
        x = std::min(x, 1919U); y = std::min(y, 1079U);
        report[offset + 1] = static_cast<std::uint8_t>(x);
        report[offset + 2] = static_cast<std::uint8_t>((x >> 8) | ((y & 15) << 4));
        report[offset + 3] = static_cast<std::uint8_t>(y >> 4);
    };
    touch(33, s.touch1Active, 0, s.touch1X, s.touch1Y);
    touch(37, s.touch2Active, 1, s.touch2X, s.touch2Y);
    // Internal VIIPER charge enums are 0/2/4; USB uses 0/1/2.
    const unsigned charging = s.chargeState == dualsense::chargeStatus::kFull ? 2 :
                              s.chargeState == dualsense::chargeStatus::kCharging ? 1 : 0;
    report[53] = static_cast<std::uint8_t>((charging << 4) | std::min<unsigned>(s.batteryPercent / 10, 10));
    report[54] = 0x08; // USB power connected, no headset.
    return report;
}

std::optional<dualsense::DualSenseFeedback> decodeUsbOutput(std::span<const std::uint8_t> report) {
    // Both 48-byte descriptor-sized and padded (63/64-byte) USB writes occur.
    if (report.size() < 48 || report.size() > 64 || report[0] != 0x02) return std::nullopt;
    dualsense::DualSenseFeedback f;
    f.enableBits1 = report[1]; f.enableBits2 = report[2];
    f.rumbleRight = report[3]; f.rumbleLeft = report[4]; f.enableBits3 = report[39];
    std::copy_n(report.begin() + 11, 11, f.rightTriggerEffect.begin());
    std::copy_n(report.begin() + 22, 11, f.leftTriggerEffect.begin());
    f.hasLightbar = (f.enableBits2 & 4) != 0;
    f.lightbarRed = report[45]; f.lightbarGreen = report[46]; f.lightbarBlue = report[47];
    return f;
}

std::vector<std::uint8_t> featureReport(std::uint8_t id, const std::array<std::uint8_t, 6>& address) {
    if (id == 0x05) {
        std::vector<std::uint8_t> report(41);
        report[0] = id;
        // Synthetic calibration matching the existing decoder's sensor units:
        // 16384 gyro counts at 1000 deg/s, 10000 acceleration counts per g.
        for (unsigned offset : {7U, 11U, 15U}) {
            put16(report, offset, 16384); put16(report, offset + 2, -16384);
        }
        put16(report, 19, 1000); put16(report, 21, 1000);
        for (unsigned offset : {23U, 27U, 31U}) {
            put16(report, offset, 10000); put16(report, offset + 2, -10000);
        }
        return report;
    }
    if (id == 0x09) {
        std::vector<std::uint8_t> report(20);
        report[0] = id;
        std::copy(address.begin(), address.end(), report.begin() + 1);
        report[7] = 0x08; report[8] = 0x25;
        return report;
    }
    if (id == 0x20) {
        // Same firmware identity as the existing patched VIIPER backend.
        return {
            0x20, 0x4A, 0x75, 0x6C, 0x20, 0x20, 0x34, 0x20,
            0x32, 0x30, 0x32, 0x35, 0x31, 0x30, 0x3A, 0x31,
            0x30, 0x3A, 0x33, 0x32, 0x03, 0x00, 0x04, 0x00,
            0x10, 0x13, 0x00, 0x00, 0x2A, 0x00, 0x10, 0x01,
            0x01, 0xC8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x30, 0x06, 0x00, 0x00,
            0x3C, 0x00, 0x01, 0x00, 0x0A, 0x00, 0x02, 0x00,
            0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
    }
    return {}; // Unknown feature requests must fail, not return fake success.
}

} // namespace asb::platform::linux_backend
