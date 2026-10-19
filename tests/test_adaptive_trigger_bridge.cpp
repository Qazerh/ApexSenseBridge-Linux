#ifdef NDEBUG
#undef NDEBUG
#endif

#include "dualsense/AdaptiveTriggerBridge.h"
#include "flydigi/Apex4Protocol.h"
#include "flydigi/Apex5Protocol.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

class FakeTransport final : public asb::platform::HidTransport {
public:
    explicit FakeTransport(bool apex4, bool wired = false) : apex4_(apex4), wired_(wired) {
        info_.vendorId = apex4 ? asb::flydigi::kApex4VendorId : asb::flydigi::kVendorId;
        info_.productId = apex4 ? asb::flydigi::kApex4ProductId : 0x2501;
        info_.usagePage = asb::flydigi::kVendorUsagePage;
        info_.inputReportLength = 32;
        info_.outputReportLength = 32;
    }

    bool isOpen() const noexcept override { return true; }
    const asb::HidDeviceInfo& info() const noexcept override { return info_; }

    bool writeOutputReport(std::span<const std::uint8_t> report,
                           std::string& error) override {
        const bool identityRequest = apex4_
            ? report.size() >= 2 && report[1] == asb::flydigi::kApex4CmdGetInfo
            : report.size() >= 4 && report[3] == asb::flydigi::kCmdGetInfo;
        if (identityRequest) {
            identityPending_ = true;
            return true;
        }
        if (failWrites) {
            error = "simulated trigger failure";
            return false;
        }
        {
            std::unique_lock lock(gateMutex_);
            if (blockNext_) {
                blockNext_ = false;
                blocked_ = true;
                gateSignal_.notify_all();
                gateSignal_.wait(lock, [this] { return released_; });
            }
        }
        if (delayNextWrite.count() != 0) {
            std::this_thread::sleep_for(std::exchange(delayNextWrite, std::chrono::milliseconds(0)));
        }
        writeTimes.push_back(std::chrono::steady_clock::now());
        writes.emplace_back(report.begin(), report.end());
        return true;
    }

    asb::platform::HidReadStatus readInputReport(
        std::span<std::uint8_t> report, std::chrono::milliseconds,
        std::size_t& bytesRead, std::string&) override {
        bytesRead = 0;
        if (!identityPending_) return asb::platform::HidReadStatus::Timeout;
        std::array<std::uint8_t, 32> reply{};
        if (apex4_) {
            reply[3] = 84;
            reply[13] = wired_ ? 1 : 0;
            reply[15] = asb::flydigi::kApex4CmdGetInfo;
        } else {
            reply[0] = asb::flydigi::kReportIdIn;
            reply[1] = asb::flydigi::kMagic0;
            reply[2] = asb::flydigi::kMagic1;
            reply[3] = asb::flydigi::kCmdGetInfo;
            reply[4] = 1;
            reply[6] = 128;
            reply[7] = wired_ ? 1 : 2;
        }
        bytesRead = (std::min)(report.size(), reply.size());
        std::copy_n(reply.begin(), bytesRead, report.begin());
        identityPending_ = false;
        return asb::platform::HidReadStatus::Data;
    }

    bool failWrites = false;
    std::chrono::milliseconds delayNextWrite{0};
    std::vector<std::vector<std::uint8_t>> writes;
    std::vector<std::chrono::steady_clock::time_point> writeTimes;

    void blockNextWrite() {
        std::lock_guard lock(gateMutex_);
        blockNext_ = true;
    }
    void waitUntilBlocked() {
        std::unique_lock lock(gateMutex_);
        assert(gateSignal_.wait_for(lock, std::chrono::seconds(2),
                                   [this] { return blocked_; }));
    }
    void releaseWrite() {
        std::lock_guard lock(gateMutex_);
        released_ = true;
        gateSignal_.notify_all();
    }

private:
    bool apex4_;
    bool wired_;
    bool identityPending_ = false;
    asb::HidDeviceInfo info_{};
    std::mutex gateMutex_;
    std::condition_variable gateSignal_;
    bool blockNext_ = false;
    bool blocked_ = false;
    bool released_ = false;
};

void expectReport(const FakeTransport& transport, bool apex4,
                  asb::TriggerSide side, asb::TriggerMode mode,
                  const std::array<std::uint8_t, 5>& params) {
    assert(!transport.writes.empty());
    const auto& report = transport.writes.back();
    const std::size_t offset = apex4 ? 6 : 8;
    if (apex4) {
        assert(report.size() == asb::flydigi::kApex4ForceTriggerReportSize);
        assert(report[0] == asb::flydigi::kApex4CommandReportId);
        assert(report[1] == asb::flydigi::kApex4CmdSetForceTriggerDInput);
        assert(report[2] == asb::flydigi::kApex4ForceTriggerEffectFamily);
        assert(report[3] == 0);
    } else {
        assert(report.size() == asb::flydigi::kReportSize);
        assert(report[0] == asb::flydigi::kReportIdOut);
        assert(report[1] == asb::flydigi::kMagic0);
        assert(report[2] == asb::flydigi::kMagic1);
        assert(report[3] == asb::flydigi::kCmdSetForceTrigger);
        assert(report[4] == 10);
        assert(report[5] == 1);
    }
    assert(report[offset - 2] == static_cast<std::uint8_t>(side));
    assert(report[offset - 1] == static_cast<std::uint8_t>(mode));
    assert(std::equal(params.begin(), params.end(), report.begin() + offset));
}

void testBridge(bool apex4) {
    using namespace asb;
    auto* transport = new FakeTransport(apex4);
    flydigi::Apex5Device device{flydigi::TransportPtr(transport)};
    std::string error;
    assert(device.verifyIdentity(error));
    assert(device.identity()->isApex4() == apex4);
    dualsense::AdaptiveTriggerBridge bridge(device);

    dualsense::DualSenseFeedback feedback{};
    feedback.enableBits1 = 0x0C;
    feedback.leftTriggerEffect = {0x26, 0xF0, 0x03, 0, 0, 0, 0, 0, 0, 35};
    feedback.rightTriggerEffect = {0x25, 0x84, 0, 3};
    bridge.handle(feedback);
    assert(transport->writes.size() == 2);
    expectReport(*transport, apex4, TriggerSide::Left, TriggerMode::RecoilRattle,
                 {77, 1, 10, 35, 0});
    const auto initialStats = bridge.stats();
    assert(initialStats.lastRightCommand->mode == TriggerMode::SniperBreak);
    assert((initialStats.lastRightCommand->params == std::array<std::uint8_t, 5>{38, 96, 32, 0, 0}));
    bridge.handle(feedback);
    assert(transport->writes.size() == 2);
    assert(bridge.stats().deduplicated == 2);

    feedback.enableBits1 = 0x01;
    feedback.rumbleLeft = 255;
    feedback.leftTriggerEffect = {5};
    bridge.handle(feedback);
    feedback.enableBits1 = 0x0D;
    feedback.leftTriggerEffect = {0x26, 0xF0, 0x03, 0, 0, 0, 0, 0, 0, 35};
    bridge.handle(feedback);
    assert(transport->writes.size() == 2);

    feedback.enableBits1 = 0x04;
    feedback.leftTriggerEffect = {5};
    feedback.rightTriggerEffect = {5};
    bridge.handle(feedback);
    assert(transport->writes.size() == 3);
    expectReport(*transport, apex4, TriggerSide::Right, TriggerMode::Normal, {});
    assert(bridge.stats().lastLeftCommand->mode == TriggerMode::RecoilRattle);

    feedback.enableBits1 = 0x08;
    feedback.leftTriggerEffect = {0x26, 0xF0, 0x03, 0, 0, 0, 0, 0, 0, 120};
    bridge.handle(feedback);
    assert(transport->writes.size() == 4);
    expectReport(*transport, apex4, TriggerSide::Left, TriggerMode::RecoilRattle,
                 {77, 1, 10, 120, 0});

    feedback.leftTriggerEffect[9] = 0;
    feedback.leftTriggerEffect[10] = 255;
    bridge.handle(feedback);
    assert(transport->writes.size() == 5);
    expectReport(*transport, apex4, TriggerSide::Left, TriggerMode::Normal, {});
    bridge.handle(feedback);
    assert(transport->writes.size() == 5);

    feedback.leftTriggerEffect = {0x26, 0xFF, 0x03, 0xFF, 0xFF, 0xFF, 0x3F, 0, 0, 35};
    bridge.handle(feedback);
    assert(transport->writes.size() == 6);
    expectReport(*transport, apex4, TriggerSide::Left, TriggerMode::RecoilRattle,
                 {0, 1, 80, 35, 0});
    feedback.leftTriggerEffect = {5};
    bridge.handle(feedback);
    assert(transport->writes.size() == 7);
    expectReport(*transport, apex4, TriggerSide::Left, TriggerMode::Normal, {});
    feedback.leftTriggerEffect = {0x26, 0xFF, 0x03, 0xFF, 0xFF, 0xFF, 0x3F, 0, 0, 35};
    bridge.handle(feedback);
    assert(transport->writes.size() == 8);
    expectReport(*transport, apex4, TriggerSide::Left, TriggerMode::RecoilRattle,
                 {0, 1, 80, 35, 0});
    feedback.leftTriggerEffect = {0x21};
    bridge.handle(feedback);
    assert(transport->writes.size() == 9);
    expectReport(*transport, apex4, TriggerSide::Left, TriggerMode::Normal, {});

    feedback.leftTriggerEffect = {1, 25, 40};
    bridge.handle(feedback);
    assert(transport->writes.size() == 10);
    expectReport(*transport, apex4, TriggerSide::Left, TriggerMode::Race, {25, 40, 0, 0, 0});
    feedback.leftTriggerEffect = {};
    bridge.handle(feedback);
    assert(bridge.stats().neutral == 1);
    assert(transport->writes.size() == 10);
    feedback.leftTriggerEffect = {1, 25, 40};
    bridge.handle(feedback);
    assert(transport->writes.size() == 10);

    feedback.leftTriggerEffect = {0x26, 0xFF, 0x83};
    bridge.handle(feedback);
    assert(transport->writes.size() == 10);
    assert(bridge.stats().lastUnsupportedLeftDualSenseType == 0x26);
    assert(bridge.stats().lastLeftDualSenseType == 1);
    feedback.leftTriggerEffect = {99};
    bridge.handle(feedback);
    assert(transport->writes.size() == 10);
    auto stats = bridge.stats();
    assert(stats.unsupported == 2);
    assert(stats.lastUnsupportedLeftDualSenseType == 99);
    assert(stats.unsupportedByType.size() == 2);
    assert(!stats.leftBlocks.empty());
    assert(stats.leftBlocks[0] == "en=0D fx=26 F0 03 00 00 00 00 00 00 23 00" ||
           stats.leftBlocks[0] == "en=0C fx=26 F0 03 00 00 00 00 00 00 23 00");

    feedback.kind = dualsense::FeedbackKind::AudioHaptics;
    bridge.handle(feedback);
    assert(transport->writes.size() == 10);
    assert(bridge.stats().lastLeftCommand->mode == TriggerMode::Race);

    feedback.kind = dualsense::FeedbackKind::HidOutput;
    feedback.leftTriggerEffect = {1, 30, 40};
    transport->failWrites = true;
    bridge.handle(feedback);
    assert(bridge.failed());
    assert(bridge.error() == "simulated trigger failure");
    assert(bridge.stats().writeFailures == 1);
    transport->failWrites = false;
    feedback.leftTriggerEffect = {1, 35, 40};
    bridge.handle(feedback);
    assert(transport->writes.size() == 10);
}

void testEmptyBlocksKeepActiveEffect(bool apex4) {
    using namespace asb;
    auto* transport = new FakeTransport(apex4);
    flydigi::Apex5Device device{flydigi::TransportPtr(transport)};
    std::string error;
    assert(device.verifyIdentity(error));
    dualsense::AdaptiveTriggerBridge bridge(device);
    dualsense::DualSenseFeedback feedback{};
    feedback.enableBits1 = 0x04;
    for (int i = 0; i < 5; ++i) {
        feedback.rightTriggerEffect = {0x25, 0x90, 0, 2};
        bridge.handle(feedback);
        feedback.rightTriggerEffect = {};
        bridge.handle(feedback);
    }
    assert(transport->writes.size() == 1);
    expectReport(*transport, apex4, TriggerSide::Right, TriggerMode::SniperBreak, {77, 57, 24, 0, 0});
    assert(bridge.stats().neutral == 5);
}

void testUnsupportedKeepsActiveEffect(bool apex4) {
    using namespace asb;
    auto* transport = new FakeTransport(apex4);
    flydigi::Apex5Device device{flydigi::TransportPtr(transport)};
    std::string error;
    assert(device.verifyIdentity(error));
    dualsense::AdaptiveTriggerBridge bridge(device);
    dualsense::DualSenseFeedback feedback{};
    feedback.enableBits1 = 0x04;
    feedback.rightTriggerEffect = {0x25, 0x84, 0, 3};
    bridge.handle(feedback);
    assert(transport->writes.size() == 1);
    feedback.rightTriggerEffect = {0xFC, 1, 2, 3};
    bridge.handle(feedback);
    assert(transport->writes.size() == 1);
    const auto stats = bridge.stats();
    assert(stats.unsupported == 1);
    assert(stats.lastUnsupportedRightDualSenseType == 0xFC);
    assert(stats.lastRightDualSenseType == 0x25);
    assert(stats.unsupportedByType.size() == 1 && stats.unsupportedByType[0].first == 0xFC);
    assert(stats.rightBlocks.size() == 2);
    assert(stats.rightBlocks[1] == "en=04 fx=FC 01 02 03 00 00 00 00 00 00 00");
    feedback.rightTriggerEffect = {0x22, 0x84, 0, 0x2B};
    bridge.handle(feedback);
    expectReport(*transport, apex4, TriggerSide::Right, TriggerMode::Race,
                 {38, 32, 0, 0, 0});
}

}

void testStrength(bool apex4) {
    using namespace asb;
    auto* transport = new FakeTransport(apex4);
    flydigi::Apex5Device device{flydigi::TransportPtr(transport)};
    std::string error;
    assert(device.verifyIdentity(error));
    dualsense::AdaptiveTriggerBridge bridge(device, 50);
    dualsense::DualSenseFeedback feedback{};
    feedback.enableBits1 = 0x08;
    feedback.leftTriggerEffect = {1, 25, 40};
    bridge.handle(feedback);
    expectReport(*transport, apex4, TriggerSide::Left, TriggerMode::Race, {25, 20, 0, 0, 0});
    feedback.leftTriggerEffect = {2, 25, 90, 60};
    bridge.handle(feedback);
    expectReport(*transport, apex4, TriggerSide::Left, TriggerMode::SniperBreak, {25, 65, 30, 0, 0});
    feedback.leftTriggerEffect = {0x25, 0x84, 0, 3};
    bridge.handle(feedback);
    expectReport(*transport, apex4, TriggerSide::Left, TriggerMode::SniperBreak, {38, 96, 16, 0, 0});
    feedback.leftTriggerEffect = {0x22, 0x01, 0x01, 0x38};
    bridge.handle(feedback);
    if (apex4) {
        expectReport(*transport, apex4, TriggerSide::Left, TriggerMode::SniperBreak, {0, 154, 4, 0, 0});
    } else {
        expectReport(*transport, apex4, TriggerSide::Left, TriggerMode::Race, {0, 4, 0, 0, 0});
    }
    feedback.leftTriggerEffect = {0x22, 0x01, 0x02, 0x39};
    bridge.handle(feedback);
    expectReport(*transport, apex4, TriggerSide::Left, TriggerMode::Race, {0, 8, 0, 0, 0});
    feedback.leftTriggerEffect = {0x26, 0xFF, 0x03, 0xFF, 0xFF, 0xFF, 0x3F, 0, 0, 35};
    bridge.handle(feedback);
    expectReport(*transport, apex4, TriggerSide::Left, TriggerMode::RecoilRattle, {0, 1, 40, 35, 0});
    dualsense::AdaptiveTriggerBridge disabled(device, 0);
    disabled.handle(feedback);
    expectReport(*transport, apex4, TriggerSide::Left, TriggerMode::Normal, {});
}

void testAsyncCoalescing() {
    using namespace asb;
    auto* transport = new FakeTransport(true);
    flydigi::Apex5Device device{flydigi::TransportPtr(transport)};
    std::string error;
    assert(device.verifyIdentity(error));
    assert(device.startAsyncWrites(error));
    dualsense::AdaptiveTriggerBridge bridge(device);
    dualsense::DualSenseFeedback feedback{};
    feedback.enableBits1 = 0x04;
    feedback.rightTriggerEffect = {1, 25, 40};
    transport->blockNextWrite();
    bridge.handle(feedback);
    transport->waitUntilBlocked();

    // Even a stalled HID write must not block feedback capture or accumulate
    // a magazine's worth of old recoil commands. Only the newest state survives.
    for (unsigned index = 0; index < 200; ++index) {
        feedback.rightTriggerEffect = {1, 25, static_cast<std::uint8_t>(index + 1)};
        bridge.handle(feedback);
        assert(device.queueRumble(static_cast<std::uint8_t>(index), 0, error));
    }
    feedback.rightTriggerEffect = {5};
    bridge.handle(feedback);
    assert(device.queueRumble(0, 0, error));
    feedback.enableBits1 = 0x08;
    feedback.leftTriggerEffect = {1, 25, 40};
    bridge.handle(feedback);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    transport->releaseWrite();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (device.asyncWriteStats().writes < 4 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    device.stopAsyncWrites();
    assert(transport->writes.size() == 4);
    unsigned left = 0, rightStop = 0, rumbleStop = 0;
    for (std::size_t index = 1; index < transport->writes.size(); ++index) {
        const auto& report = transport->writes[index];
        if (report[1] == flydigi::kApex4CmdRumble) {
            assert(report[2] == 0 && report[3] == 0);
            ++rumbleStop;
        } else if (report[4] == static_cast<std::uint8_t>(TriggerSide::Right)) {
            assert(report[5] == static_cast<std::uint8_t>(TriggerMode::Normal));
            ++rightStop;
        } else {
            assert(report[5] == static_cast<std::uint8_t>(TriggerMode::Race));
            ++left;
        }
    }
    assert(left == 1 && rightStop == 1 && rumbleStop == 1);
    const auto stats = device.asyncWriteStats();
    assert(stats.writes == 4);
    assert(stats.coalesced >= 398);
    assert(stats.slowWrites >= 1);
    assert(stats.maximumWriteUs >= 100000);
    assert(stats.maximumQueueUs >= 100000);
    assert(device.asyncWriteRetries() == 0);
    for (std::size_t index = 0; index + 1 < transport->writeTimes.size(); ++index) {
        assert(transport->writeTimes[index + 1] - transport->writeTimes[index] >=
               std::chrono::milliseconds(25));
    }
    assert(device.triggerWriteStats().writes == 0);
    assert(device.rumbleWriteStats().writes == 0);
}

void testWriterSelection() {
    using namespace asb;
    struct Case {
        bool apex4;
        bool wired;
        bool async;
        std::string_view connection;
    };
    for (const auto& entry : {Case{true, false, true, "2.4 GHz receiver"},
                              Case{true, true, true, "wired"},
                              Case{false, false, true, "2.4 GHz receiver"},
                              Case{false, true, false, "wired"}}) {
        auto* transport = new FakeTransport(entry.apex4, entry.wired);
        flydigi::Apex5Device device{flydigi::TransportPtr(transport)};
        std::string error;
        assert(device.verifyIdentity(error));
        assert(device.identity()->isWired() == entry.wired);
        assert(device.identity()->connectionName() == entry.connection);
        const auto policy = device.asyncWritePolicy();
        assert(policy.has_value() == entry.async);
        if (entry.apex4) {
            assert(policy->spacing == std::chrono::milliseconds(25));
            assert(policy->rumbleHoldoffAfterTrigger == std::chrono::milliseconds(0));
        } else if (entry.async) {
            assert(policy->spacing == std::chrono::milliseconds(10));
            assert(policy->rumbleHoldoffAfterTrigger == std::chrono::milliseconds(30));
        }
        assert(device.startAsyncWrites(error));
        assert(device.asyncWritesRunning() == entry.async);
        device.stopAsyncWrites();
        assert(!device.asyncWritesRunning());
    }
}

void testApex5WiredStaysSynchronous() {
    using namespace asb;
    auto* transport = new FakeTransport(false, true);
    flydigi::Apex5Device device{flydigi::TransportPtr(transport)};
    std::string error;
    assert(device.verifyIdentity(error));
    assert(device.startAsyncWrites(error));
    assert(!device.asyncWritesRunning());
    assert(device.queueTriggerRaw({TriggerSide::Right, TriggerMode::Race, {25, 40}}, error));
    assert(transport->writes.size() == 1);
    assert(transport->writes.back()[3] == flydigi::kCmdSetForceTrigger);
    assert(device.queueRumble(10, 20, error));
    assert(transport->writes.size() == 2);
    assert(transport->writes.back()[3] == flydigi::kCmdSetRumble);
    assert(transport->writes.back()[5] == 10 && transport->writes.back()[6] == 20);
    assert(device.asyncWriteStats().writes == 0);
    assert(device.triggerWriteStats().writes == 1);
    assert(device.rumbleWriteStats().writes == 1);
}

void testApex5ReceiverPrioritizesTriggers() {
    using namespace asb;
    auto* transport = new FakeTransport(false);
    flydigi::Apex5Device device{flydigi::TransportPtr(transport)};
    std::string error;
    assert(device.verifyIdentity(error));
    assert(device.startAsyncWrites(error));
    assert(device.asyncWritesRunning());

    ForceTriggerCommand right{TriggerSide::Right, TriggerMode::Race, {25, 40}};
    transport->blockNextWrite();
    assert(device.queueTriggerRaw(right, error));
    transport->waitUntilBlocked();
    for (unsigned index = 1; index <= 100; ++index) {
        right.params[1] = static_cast<std::uint8_t>(index);
        assert(device.queueTriggerRaw(right, error));
    }
    assert(device.queueTriggerRaw(
        {TriggerSide::Left, TriggerMode::RecoilRattle, {0, 1, 40, 4}}, error));
    const auto rumbleQueuedAt = std::chrono::steady_clock::now();
    for (unsigned index = 1; index <= 100; ++index) {
        assert(device.queueRumble(static_cast<std::uint8_t>(index), 7, error));
    }
    transport->releaseWrite();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (device.asyncWriteStats().writes < 4 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    device.stopAsyncWrites();

    assert(transport->writes.size() == 4);
    const auto& writes = transport->writes;
    assert(writes[0][3] == flydigi::kCmdSetForceTrigger);
    assert(writes[0][6] == static_cast<std::uint8_t>(TriggerSide::Right) && writes[0][9] == 40);
    assert(writes[1][3] == flydigi::kCmdSetForceTrigger);
    assert(writes[1][6] == static_cast<std::uint8_t>(TriggerSide::Left));
    assert(writes[1][7] == static_cast<std::uint8_t>(TriggerMode::RecoilRattle));
    assert(writes[2][3] == flydigi::kCmdSetForceTrigger);
    assert(writes[2][6] == static_cast<std::uint8_t>(TriggerSide::Right) && writes[2][9] == 100);
    assert(writes[3][3] == flydigi::kCmdSetRumble);
    assert(writes[3][5] == 100 && writes[3][6] == 7);

    const auto& times = transport->writeTimes;
    assert(times[1] - times[0] >= std::chrono::milliseconds(10));
    assert(times[2] - times[1] >= std::chrono::milliseconds(10));
    assert(times[3] - times[2] >= std::chrono::milliseconds(30) ||
           times[3] - rumbleQueuedAt >= std::chrono::milliseconds(100));

    const auto stats = device.asyncWriteStats();
    assert(stats.writes == 4);
    assert(stats.coalesced >= 198);
    assert(device.asyncWriteRetries() == 0);
    assert(device.triggerWriteStats().writes == 3);
    assert(device.rumbleWriteStats().writes == 1);
}

void testApex5WriteTelemetry() {
    using namespace asb;
    auto* transport = new FakeTransport(false, true);
    flydigi::Apex5Device device{flydigi::TransportPtr(transport)};
    std::string error;
    assert(device.verifyIdentity(error));
    assert(device.triggerWriteStats().writes == 0);
    transport->delayNextWrite = std::chrono::milliseconds(25);
    assert(device.setTriggerRaw({TriggerSide::Left, TriggerMode::Race, {25, 40}}, error));
    assert(device.setRumble(50, 60, error));
    assert(device.clearAll(error));
    const auto triggers = device.triggerWriteStats();
    assert(triggers.writes == 3);
    assert(triggers.slowWrites >= 1);
    assert(triggers.maximumWriteUs >= 20000);
    const auto rumble = device.rumbleWriteStats();
    assert(rumble.writes == 1);
    transport->failWrites = true;
    assert(!device.setRumble(1, 2, error));
    assert(device.rumbleWriteStats().writes == 2);
}

void testBowEmulation() {
    using namespace asb;
    auto* transport = new FakeTransport(false);
    flydigi::Apex5Device device{flydigi::TransportPtr(transport)};
    std::string error;
    assert(device.verifyIdentity(error));
    dualsense::AdaptiveTriggerBridge bridge(device);
    dualsense::DualSenseFeedback feedback{};
    feedback.enableBits1 = 0x08;
    feedback.leftTriggerEffect = {0x22, 0x44, 0x00, 0x3B};

    bridge.handle(feedback);
    assert(transport->writes.size() == 1);
    expectReport(*transport, false, TriggerSide::Left, TriggerMode::Race, {38, 32, 0, 0, 0});
    bridge.handle(feedback);
    assert(transport->writes.size() == 1);

    bridge.updateTriggerPositions(128, 255);
    assert(transport->writes.size() == 1);
    bridge.updateTriggerPositions(129, 255);
    assert(transport->writes.size() == 2);
    expectReport(*transport, false, TriggerSide::Left, TriggerMode::SniperBreak, {115, 40, 32, 0, 0});
    bridge.updateTriggerPositions(255, 0);
    bridge.handle(feedback);
    assert(transport->writes.size() == 2);
    bridge.updateTriggerPositions(41, 0);
    assert(transport->writes.size() == 2);
    bridge.updateTriggerPositions(40, 0);
    assert(transport->writes.size() == 3);
    expectReport(*transport, false, TriggerSide::Left, TriggerMode::Race, {38, 32, 0, 0, 0});
    bridge.updateTriggerPositions(200, 0);
    assert(transport->writes.size() == 4);
    expectReport(*transport, false, TriggerSide::Left, TriggerMode::SniperBreak, {115, 40, 32, 0, 0});
    assert(bridge.stats().lastLeftCommand->mode == TriggerMode::SniperBreak);

    feedback.leftTriggerEffect = {0x21, 0xFC, 0x03};
    bridge.handle(feedback);
    assert(transport->writes.size() == 5);
    expectReport(*transport, false, TriggerSide::Left, TriggerMode::Race, {38, 8, 0, 0, 0});
    bridge.updateTriggerPositions(255, 255);
    bridge.updateTriggerPositions(0, 0);
    assert(transport->writes.size() == 5);
    assert(!bridge.failed());

    feedback.enableBits1 = 0x04;
    feedback.rightTriggerEffect = {0x22, 0x44, 0x00, 0x0B};
    bridge.handle(feedback);
    assert(transport->writes.size() == 6);
    expectReport(*transport, false, TriggerSide::Right, TriggerMode::Race, {38, 32, 0, 0, 0});
    bridge.updateTriggerPositions(255, 255);
    assert(transport->writes.size() == 6);
}

void testBowEmulationDisabledOnApex4() {
    using namespace asb;
    auto* transport = new FakeTransport(true);
    flydigi::Apex5Device device{flydigi::TransportPtr(transport)};
    std::string error;
    assert(device.verifyIdentity(error));
    dualsense::AdaptiveTriggerBridge bridge(device);
    dualsense::DualSenseFeedback feedback{};
    feedback.enableBits1 = 0x08;
    feedback.leftTriggerEffect = {0x22, 0x44, 0x00, 0x3B};
    bridge.handle(feedback);
    assert(transport->writes.size() == 1);
    expectReport(*transport, true, TriggerSide::Left, TriggerMode::SniperBreak, {38, 77, 32, 0, 0});
    bridge.updateTriggerPositions(255, 255);
    assert(transport->writes.size() == 1);
}

int main() {
    {
        // A runtime can redirect diagnostics away from its input thread while
        // retaining block deduplication and all trigger translations.
        auto* transport = new FakeTransport(false);
        asb::flydigi::Apex5Device device{asb::flydigi::TransportPtr(transport)};
        std::string error;
        assert(device.verifyIdentity(error));
        asb::dualsense::AdaptiveTriggerBridge bridge(device);
        std::vector<std::string> messages;
        bridge.setLogHandler([&](std::string_view line) { messages.emplace_back(line); });
        asb::dualsense::DualSenseFeedback feedback{};
        feedback.enableBits1 = 8; feedback.leftTriggerEffect = {0x21, 0xfc, 3};
        bridge.handle(feedback); bridge.handle(feedback);
        assert(messages.size() == 1 && messages.front().find("[triggers] lt block #1") == 0);
        assert(bridge.stats().translated == 1 && bridge.stats().leftBlocks.size() == 1);
    }
    testBowEmulation();
    testBowEmulationDisabledOnApex4();
    testBridge(false);
    testBridge(true);
    testUnsupportedKeepsActiveEffect(false);
    testEmptyBlocksKeepActiveEffect(false);
    testUnsupportedKeepsActiveEffect(true);
    testEmptyBlocksKeepActiveEffect(true);
    testStrength(false);
    testStrength(true);
    testAsyncCoalescing();
    testWriterSelection();
    testApex5WiredStaysSynchronous();
    testApex5ReceiverPrioritizesTriggers();
    testApex5WriteTelemetry();
}
