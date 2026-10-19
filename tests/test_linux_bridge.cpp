#ifdef NDEBUG
#undef NDEBUG
#endif

#include "dualsense/AdaptiveTriggerBridge.h"
#include "dualsense/LightbarBridge.h"
#include "dualsense/RumbleBridge.h"
#include "flydigi/Apex5Input.h"
#include "platform/linux/DualSenseReports.h"
#include "platform/linux/BridgeLifecycle.h"
#include "platform/linux/HidReportDescriptor.h"
#include "platform/linux/InputStream.h"
#include "platform/linux/UhidDevice.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <deque>
#include <future>
#include <iostream>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

namespace {
using namespace asb::platform::linux_backend;

// The real translator and device identity guard are used; only USB is faked.
class ApexTransport final : public asb::platform::HidTransport {
public:
    ApexTransport() {
        info_.vendorId = 0x37d7; info_.productId = 0x2501;
        info_.inputReportLength = 32; info_.outputReportLength = 32;
        rgbConfig = asb::flydigi::buildStaticRgbPayload(32, 48, 64, 70);
    }
    bool isOpen() const noexcept override { return true; }
    const asb::HidDeviceInfo& info() const noexcept override { return info_; }
    bool writeOutputReport(std::span<const std::uint8_t> report, std::string& error) override {
        if (report[3] == 1) {
            replies_.push_back({4, 0x5a, 0xa5, 1, 0, 0, 128, 1});
            replies_.back().resize(32);
            return true;
        }
        if (fail) { error = "device unplugged"; return false; }
        if (report[3] == asb::flydigi::kCmdWriteRgbPack && failRgbOnce.exchange(false)) {
            error = "simulated RGB failure";
            return false;
        }
        writes.emplace_back(report.begin(), report.end());
        const auto command = report[3];
        if ((command == asb::flydigi::kCmdWriteRgbStart || command == asb::flydigi::kCmdWriteRgbPack) && rgbDelayMs)
            std::this_thread::sleep_for(std::chrono::milliseconds(rgbDelayMs.load()));
        if (command == asb::flydigi::kCmdSetRumble) {
            lastLow = report[5]; lastHigh = report[6]; ++rumbleWrites;
        }
        if (command == 0x51) ++triggerWrites;
        if (command == asb::flydigi::kCmdProfileStatus || command == asb::flydigi::kCmdReadInputTransport ||
            command == asb::flydigi::kCmdSetInputTransport) {
            if (command == asb::flydigi::kCmdSetInputTransport) {
                if (failRouting) { error = "lost routing ACK"; return false; }
                controllerData = report[5]; rawData = report[6];
            }
            std::vector<std::uint8_t> reply(32);
            reply[0] = 4; reply[1] = 0x5a; reply[2] = 0xa5; reply[3] = command;
            if (command == asb::flydigi::kCmdProfileStatus) reply[6] = profile;
            else { reply[6] = controllerData; reply[7] = rawData; }
            replies_.push_back(std::move(reply));
        } else if (command == asb::flydigi::kCmdReadRgbConfig) {
            for (unsigned packet = 0; packet < asb::flydigi::kRgbPacketCount; ++packet) {
                std::vector<std::uint8_t> reply(32);
                reply[0] = 4; reply[1] = 0x5a; reply[2] = 0xa5; reply[3] = command;
                reply[4] = 23; reply[5] = packet;
                std::copy_n(rgbConfig.begin() + packet * 20, 20, reply.begin() + 7);
                replies_.push_back(std::move(reply));
            }
        } else if (command == asb::flydigi::kCmdWriteRgbStart ||
                   command == asb::flydigi::kCmdWriteRgbPack ||
                   command == asb::flydigi::kCmdApplyProfile) {
            if (command == asb::flydigi::kCmdApplyProfile) profile = report[5];
            if (command == asb::flydigi::kCmdWriteRgbStart) {
                rgbStart_ = report[6];
                if (blockRgbWrite.exchange(false)) {
                    rgbWriteBlocked = true;
                    while (!releaseRgbWrite) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            } else if (command == asb::flydigi::kCmdWriteRgbPack) {
                std::copy_n(report.begin() + 6, 20, rgbConfig.begin() + (rgbStart_ + report[5]) * 20);
            }
            std::vector<std::uint8_t> reply(32);
            reply[0] = 4; reply[1] = 0x5a; reply[2] = 0xa5; reply[3] = command;
            replies_.push_back(std::move(reply));
        }
        return true;
    }
    asb::platform::HidReadStatus readInputReport(std::span<std::uint8_t> report,
        std::chrono::milliseconds, std::size_t& size, std::string&) override {
        size = 0;
        if (replies_.empty()) return asb::platform::HidReadStatus::Timeout;
        const auto reply = std::move(replies_.front());
        replies_.pop_front();
        std::copy(reply.begin(), reply.end(), report.begin());
        size = reply.size();
        return asb::platform::HidReadStatus::Data;
    }
    bool fail = false, failRouting = false;
    std::uint8_t profile = 0, controllerData = 1, rawData = 0;
    std::atomic_bool blockRgbWrite{false}, rgbWriteBlocked{false}, releaseRgbWrite{false}, failRgbOnce{false};
    std::atomic_uint rumbleWrites{0}, triggerWrites{0}, lastLow{0}, lastHigh{0};
    std::atomic_uint rgbDelayMs{0};
    std::array<std::uint8_t, asb::flydigi::kRgbConfigSize> rgbConfig;
    std::vector<std::vector<std::uint8_t>> writes;
private:
    asb::HidDeviceInfo info_;
    std::deque<std::vector<std::uint8_t>> replies_;
    unsigned rgbStart_ = 0;
};

void sendEvent(int fd, const uhid_event& event) {
    const auto sent = ::send(fd, &event, sizeof(event), 0);
    if (sent != sizeof(event)) std::cerr << "send event type=" << event.type << " fd=" << fd
                                       << " result=" << sent << " errno=" << errno << '\n';
    assert(sent == sizeof(event));
}
uhid_event receive(int fd) {
    uhid_event event{};
    assert(::recv(fd, &event, sizeof(event), MSG_DONTWAIT) >= 4);
    return event;
}

void descriptorTests() {
    const auto layout = parseHidReportDescriptor(dualSenseDescriptor());
    assert(layout && layout->usagePage == 1 && layout->usage == 5);
    assert(layout->inputLength == 64 && layout->outputLength == 48 && layout->featureLength == 64);
    // Vendor report IDs 4/3 each carry 31 data bytes plus their ID.
    const std::vector<std::uint8_t> vendor{
        0x06, 0xa0, 0xff, 0x09, 1, 0xa1, 1, 0x75, 8, 0x95, 31,
        0x85, 4, 0x81, 2, 0x85, 3, 0x91, 2, 0xc0};
    const auto apex = parseHidReportDescriptor(vendor);
    assert(apex && apex->usagePage == 0xffa0 && apex->inputLength == 32 && apex->outputLength == 32);
    const std::vector<std::uint8_t> unnumbered{5, 1, 9, 5, 0xa1, 1, 0x75, 8, 0x95, 32, 0x81, 2, 0xc0};
    assert(parseHidReportDescriptor(unnumbered)->inputLength == 32);
    // Truncation, stack underflow and unbounded report counts are rejected.
    auto broken = vendor;
    broken.pop_back();
    assert(!parseHidReportDescriptor(broken));
    assert(!parseHidReportDescriptor(std::array<std::uint8_t, 1>{0xb4}));
    assert(!parseHidReportDescriptor(std::array<std::uint8_t, 2>{0x77, 0}));
    assert(!parseHidReportDescriptor(std::array<std::uint8_t, 2>{0xfe, 10}));
    broken = vendor;
    broken[8] = 255; broken[10] = 255;
    assert(!parseHidReportDescriptor(broken));
}

void reportTests() {
    const auto neutral = buildUsbInput({}, 0xff, 0x12345678);
    assert(neutral[0] == 1 && neutral[1] == 128 && neutral[8] == 8);
    assert(neutral[33] == 0x80 && neutral[37] == 0x81);
    assert(neutral[28] == 0x78 && neutral[31] == 0x12);
    constexpr std::array<unsigned, 8> directions{1, 9, 8, 10, 2, 6, 4, 5};
    for (unsigned i = 0; i < directions.size(); ++i) {
        asb::dualsense::DualSenseInputState state;
        state.dpad = directions[i];
        assert(buildUsbInput(state, 0, 0)[8] == i);
    }
    // A real-format Apex operator packet crosses the entire input adapter.
    std::array<std::uint8_t, 32> operatorReport{4, 0x5a, 0xa5, 0xef};
    operatorReport[12] = 0x10; // A -> Cross
    operatorReport[13] = 0x02; // Start -> Options
    operatorReport[16] = 123; operatorReport[17] = 234;
    auto state = asb::flydigi::decodeApex5InputReport(operatorReport);
    assert(state);
    auto wire = buildUsbInput(*state, 42, 0);
    assert(wire[5] == 123 && wire[6] == 234 && wire[7] == 42);
    assert(wire[8] == 0x28 && (wire[9] & 0x2c) == 0x2c);
    state->chargeState = asb::dualsense::chargeStatus::kFull;
    state->touch1Active = true; state->touch1X = 1919; state->touch1Y = 1079;
    wire = buildUsbInput(*state, 0, 0);
    assert(wire[53] == 0x2a && wire[34] == 0x7f && wire[35] == 0x77 && wire[36] == 0x43);
    assert(!decodeUsbOutput(std::array<std::uint8_t, 47>{2}));
    assert(!decodeUsbOutput(std::array<std::uint8_t, 78>{0x31}));
    assert(!decodeUsbOutput(std::array<std::uint8_t, 48>{1}));
    const auto calibration = featureReport(5, {});
    assert(calibration.size() == 41 && calibration[7] == 0 && calibration[8] == 0x40);
    assert(calibration[23] == 0x10 && calibration[24] == 0x27); // +1g = 10000
    assert(featureReport(0x20, {}).size() == 64);
    assert(featureReport(0xff, {}).empty());

    // Physical six-axis samples must survive decoding and the USB report layout,
    // including negative rotation and acceleration conversion (4096 counts/g).
    const auto put = [&](unsigned offset, std::int16_t value) {
        operatorReport[offset] = static_cast<std::uint16_t>(value);
        operatorReport[offset + 1] = static_cast<std::uint16_t>(value) >> 8;
    };
    put(18, -16384); put(20, 8192); put(22, 32767);
    put(24, 4096); put(26, -4096); put(28, 0);
    state = asb::flydigi::decodeApex5InputReport(operatorReport);
    const auto original = *state;
    applyGyroStrength(*state, 100);
    assert(*state == original);
    applyGyroStrength(*state, 200);
    wire = buildUsbInput(*state, 3, 3000);
    const auto get = [&](unsigned offset) {
        return static_cast<std::int16_t>(wire[offset] | unsigned(wire[offset + 1]) << 8);
    };
    assert(get(16) == -32768 && get(18) == 16384 && get(20) == 32767);
    assert(get(22) == 10000 && get(24) == -10000 && get(26) == 0);
    applyGyroStrength(*state, 0);
    assert(!state->gyroX && !state->gyroY && !state->gyroZ);
    assert(state->accelX == 10000 && state->accelY == -10000);
    // A sub-100% gain retains negative signs instead of unsigned wraparound.
    *state = original;
    applyGyroStrength(*state, 25);
    assert(state->gyroX == -4096 && state->gyroY == 2048);
    for (unsigned length : {48U, 63U, 64U}) {
        std::vector<std::uint8_t> color(length);
        color[0] = 2; color[2] = 4;
        color[45] = 12; color[46] = 34; color[47] = 56;
        auto feedback = decodeUsbOutput(color);
        assert(feedback && feedback->hasLightbarColor());
        assert(feedback->lightbarRed == 12 && feedback->lightbarGreen == 34 && feedback->lightbarBlue == 56);
        color[2] = 0;
        assert(!decodeUsbOutput(color)->hasLightbarColor());
        color[2] = 4; color[45] = color[46] = color[47] = 0;
        assert(decodeUsbOutput(color)->hasLightbarColor()); // Black switches the LEDs off.
    }
}

void rgbTests() {
    using namespace std::chrono_literals;
    int sockets[2];
    assert(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0);
    UhidDevice pad(sockets[0]);
    auto transport = std::make_unique<ApexTransport>();
    auto* usb = transport.get();
    asb::flydigi::Apex5Device apex(asb::flydigi::TransportPtr(transport.release()));
    std::string error;
    assert(apex.verifyIdentity(error));
    const auto originalRgb = usb->rgbConfig;
    asb::dualsense::AdaptiveTriggerBridge triggers(apex);
    asb::dualsense::LightbarBridge lights(apex, 0, {20ms, true});
    assert(!lights.failed());
    const auto feedback = [&](const auto& data) { triggers.handle(data); lights.handle(data); };
    uhid_event event{};
    event.type = UHID_OUTPUT; event.u.output.rtype = UHID_OUTPUT_REPORT;
    event.u.output.size = 48;
    event.u.output.data[0] = 2; event.u.output.data[2] = 4;
    event.u.output.data[45] = 10; event.u.output.data[46] = 20; event.u.output.data[47] = 30;
    usb->blockRgbWrite = true;
    sendEvent(sockets[1], event);
    assert(pad.dispatch(feedback, error));
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!usb->rgbWriteBlocked && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    assert(usb->rgbWriteBlocked);
    // Raw motion may not steal the RGB transaction's command replies.
    std::promise<void> readerStarted;
    auto readerReady = readerStarted.get_future();
    auto reader = std::async(std::launch::async, [&] {
        std::array<std::uint8_t, 32> report{};
        std::size_t size = 0;
        std::string readError;
        readerStarted.set_value();
        return apex.readRawInputReport(report, 0ms, size, readError);
    });
    readerReady.wait();
    const auto readerStatus = reader.wait_for(40ms);
    usb->releaseRgbWrite = true;
    assert(readerStatus == std::future_status::timeout);
    assert(reader.get() == asb::platform::HidReadStatus::Timeout);
    deadline = std::chrono::steady_clock::now() + 2s;
    while (!lights.failed() && lights.stats().writes < 1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    assert(!lights.failed() && lights.stats().writes == 1 && !triggers.failed());
    assert(triggers.stats().translated == 0);
    assert(usb->rgbConfig[2] == 0); // The completed color is made visible.
    assert(usb->rgbConfig[4] == 0 && usb->rgbConfig[8] == 5);
    for (unsigned offset = 20; offset < 56; offset += 3)
        assert(usb->rgbConfig[offset] == 10 && usb->rgbConfig[offset + 1] == 20 && usb->rgbConfig[offset + 2] == 30);
    assert(std::equal(usb->rgbConfig.begin() + 56, usb->rgbConfig.end(), originalRgb.begin() + 56));
    unsigned packs = 0;
    for (const auto& packet : usb->writes) if (packet[3] == asb::flydigi::kCmdWriteRgbPack) ++packs;
    assert(packs == 4); // Header+frame (3), release header (1), instead of 20.
    sendEvent(sockets[1], event);
    assert(pad.dispatch(feedback, error));
    assert(lights.stats().deduplicated == 1);
    // A control-channel update can carry lighting and both trigger effects.
    event = {};
    event.type = UHID_SET_REPORT;
    event.u.set_report.id = 901; event.u.set_report.rtype = UHID_OUTPUT_REPORT;
    event.u.set_report.rnum = 2; event.u.set_report.size = 48;
    auto* data = event.u.set_report.data;
    data[0] = 2; data[1] = 0x0c; data[2] = 4;
    data[11] = data[22] = 0x21; data[12] = data[23] = 4; data[14] = data[25] = 0x40;
    data[45] = 50; data[46] = 60; data[47] = 70;
    sendEvent(sockets[1], event);
    assert(pad.dispatch(feedback, error));
    assert(receive(sockets[1]).u.set_report_reply.err == 0);
    assert(!triggers.failed() && triggers.stats().active == 2);
    deadline = std::chrono::steady_clock::now() + 2s;
    while (!lights.failed() && lights.stats().writes < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    assert(!lights.failed() && lights.stats().writes == 2);
    assert(usb->rgbConfig[20] == 50 && usb->rgbConfig[21] == 60 && usb->rgbConfig[22] == 70);
    // Small changes and black must not disappear in a color dead zone.
    data[1] = 0; data[45] = data[46] = data[47] = 1;
    sendEvent(sockets[1], event);
    assert(pad.dispatch(feedback, error)); (void)receive(sockets[1]);
    deadline = std::chrono::steady_clock::now() + 2s;
    while (!lights.failed() && lights.stats().writes < 3 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    assert(lights.stats().writes == 3);
    data[45] = data[46] = data[47] = 0;
    sendEvent(sockets[1], event);
    assert(pad.dispatch(feedback, error)); (void)receive(sockets[1]);
    deadline = std::chrono::steady_clock::now() + 2s;
    while (!lights.failed() && lights.stats().writes < 4 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    assert(!lights.failed() && lights.stats().writes == 4 && usb->rgbConfig[20] == 0);
    // Replay the tester's 33ms animation cadence with a 2ms USB round trip
    // per RGB command. The old 19-packet/80ms path cannot follow this stream.
    const auto beforeAnimation = lights.stats().writes;
    usb->rgbDelayMs = 2;
    for (unsigned frame = 0; frame < 10; ++frame) {
        data[45] = 20 + frame * 3; data[46] = 50; data[47] = 200;
        sendEvent(sockets[1], event);
        assert(pad.dispatch(feedback, error)); (void)receive(sockets[1]);
        std::this_thread::sleep_for(33ms);
    }
    assert(!lights.failed() && lights.stats().writes >= beforeAnimation + 8);
    usb->rgbDelayMs = 0;
    // Lighting restoration must be byte-for-byte, not just a default color.
    lights.restore();
    assert(!lights.failed() && usb->rgbConfig == originalRgb);
    assert(std::any_of(usb->writes.begin(), usb->writes.end(), [](const auto& packet) {
        return packet[3] == asb::flydigi::kCmdApplyProfile;
    }));
    // Surface worker errors, then still restore the saved configuration.
    asb::dualsense::LightbarBridge interrupted(apex);
    assert(!interrupted.failed());
    usb->failRgbOnce = true;
    interrupted.handle(*decodeUsbOutput({event.u.set_report.data, 48}));
    deadline = std::chrono::steady_clock::now() + 2s;
    while (!interrupted.failed() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    assert(interrupted.failed() && interrupted.error().find("simulated RGB failure") != std::string::npos);
    interrupted.restore();
    assert(interrupted.stats().writeFailures == 1 && usb->rgbConfig == originalRgb);
    ::close(sockets[1]);
}

void reconnectSettingsTests() {
    using namespace std::chrono_literals;
    auto transport = std::make_unique<ApexTransport>(); auto* usb = transport.get();
    asb::flydigi::Apex5Device apex(asb::flydigi::TransportPtr(transport.release()));
    std::string error; assert(apex.verifyIdentity(error));
    const auto baseline = usb->rgbConfig;
    DeferredSettings saved;
    saved.pending = true; saved.model = 128; saved.originalSlot = 1; saved.rgbSlot = 3;
    saved.transport.controllerData = true; saved.transport.rawData = false;
    {
        asb::dualsense::LightbarBridge lights(apex, 3);
        assert(!lights.failed() && lights.backup() == baseline);
        saved.rgb = lights.backup();
        asb::dualsense::DualSenseFeedback f; f.hasLightbar = true; f.lightbarRed = 200;
        lights.handle(f);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!lights.stats().writes && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
        lights.stopWithoutRestore();
        assert(lights.stats().writes == 1 && usb->rgbConfig != baseline);
    }
    assert(usb->rgbConfig != baseline); // Destructor must not touch absent hardware.
    usb->profile = 3; usb->rawData = 1; usb->fail = true;
    assert(!saved.recover(apex, error) && saved.pending && saved.rgb == baseline);
    usb->fail = false; usb->failRouting = true; error.clear();
    assert(!saved.recover(apex, error) && saved.pending && saved.rgb == baseline);
    assert(usb->rgbConfig == baseline && usb->profile == 1);
    usb->failRouting = false; error.clear();
    assert(saved.recover(apex, error) && !saved.pending && !saved.rgb);
    assert(usb->rgbConfig == baseline && usb->profile == 1 && usb->controllerData && !usb->rawData);
    const auto writes = usb->writes.size();
    assert(saved.recover(apex, error) && usb->writes.size() == writes);
    saved.pending = true; saved.model = 134;
    assert(!saved.recover(apex, error) && saved.pending && usb->writes.size() == writes);
}

void busyFeedbackInputTests() {
    using namespace std::chrono_literals;
    int hostSockets[2], inputSockets[2];
    assert(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, hostSockets) == 0);
    assert(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, inputSockets) == 0);
    UhidDevice pad(hostSockets[0]);
    InputStream inputStream(inputSockets[0]);
    auto transport = std::make_unique<ApexTransport>();
    auto* usb = transport.get();
    asb::flydigi::Apex5Device apex(asb::flydigi::TransportPtr(transport.release()));
    std::string error;
    assert(apex.verifyIdentity(error));
    // The override queues wired feedback too, just as LinuxMain does.
    assert(apex.startAsyncWrites(error, asb::flydigi::AsyncWritePolicy{1ms, 0ms}));
    asb::dualsense::AdaptiveTriggerBridge triggers(apex);
    asb::dualsense::RumbleBridge rumble(apex);
    asb::dualsense::LightbarBridge lights(apex, 0, {20ms, true});
    const auto handler = [&](const auto& data) { triggers.handle(data); rumble.handle(data); lights.handle(data); };
    uhid_event event{};
    event.type = UHID_OUTPUT; event.u.output.rtype = UHID_OUTPUT_REPORT; event.u.output.size = 48;
    auto* data = event.u.output.data;
    data[0] = 2; data[2] = 4; data[45] = 100;
    usb->blockRgbWrite = true;
    sendEvent(hostSockets[1], event);
    assert(pad.dispatch(handler, error));
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!usb->rgbWriteBlocked && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    assert(usb->rgbWriteBlocked);
    // A page already sending colors can also send motors/triggers immediately.
    data[1] = 0x0f; data[3] = 40; data[4] = 80;
    data[11] = data[22] = 0x21; data[12] = data[23] = 4; data[14] = data[25] = 0x40;
    sendEvent(hostSockets[1], event);
    auto dispatch = std::async(std::launch::async, [&] { return pad.dispatch(handler, error); });
    const auto ready = dispatch.wait_for(100ms);
    if (ready != std::future_status::ready) usb->releaseRgbWrite = true;
    assert(ready == std::future_status::ready && dispatch.get());
    // Keep real-format stick/button/motion reports flowing beyond the 1.5s
    // stale-input deadline while the RGB command channel remains blocked.
    unsigned reports = 0;
    const auto until = std::chrono::steady_clock::now() + 1600ms;
    while (std::chrono::steady_clock::now() < until) {
        std::array<std::uint8_t, 32> raw{4, 0x5a, 0xa5, 0xef};
        raw[5] = 0x40; raw[12] = reports % 2 ? 0x10 : 0; raw[18] = 0x34; raw[19] = 0x12;
        assert(::send(inputSockets[1], raw.data(), raw.size(), 0) == static_cast<ssize_t>(raw.size()));
        std::array<std::uint8_t, 32> packet{};
        std::size_t size = 0;
        assert(inputStream.read(packet, 20ms, size, error) == asb::platform::HidReadStatus::Data);
        const auto state = asb::flydigi::decodeApex5InputReport({packet.data(), size});
        assert(state && state->gyroX == 0x1234);
        assert(pad.update(*state, error));
        const auto usbInput = receive(hostSockets[1]);
        assert(usbInput.type == UHID_INPUT2 && usbInput.u.input2.data[1] > 128);
        assert((usbInput.u.input2.data[8] & 0x20) == (reports % 2 ? 0x20 : 0));
        data[45] = 100 + reports % 100;
        sendEvent(hostSockets[1], event);
        assert(pad.dispatch(handler, error));
        ++reports;
        std::this_thread::sleep_for(5ms);
    }
    assert(reports >= 100 && lights.stats().writes == 0);
    usb->releaseRgbWrite = true;
    deadline = std::chrono::steady_clock::now() + 2s;
    while ((usb->rumbleWrites < 1 || usb->triggerWrites < 2 || lights.stats().writes < 2) &&
           std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    assert(!lights.failed() && !rumble.failed() && !triggers.failed());
    assert(usb->lastLow == 80 && usb->lastHigh == 40 && usb->triggerWrites == 2);
    data[1] = 3; data[2] = 0; data[3] = data[4] = 0;
    sendEvent(hostSockets[1], event);
    assert(pad.dispatch(handler, error));
    deadline = std::chrono::steady_clock::now() + 2s;
    while (usb->rumbleWrites < 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    assert(usb->lastLow == 0 && usb->lastHigh == 0 && rumble.stats().stops == 1);
    apex.stopAsyncWrites();
    lights.restore();
    assert(!lights.failed());
    ::close(inputSockets[1]);
    std::array<std::uint8_t, 32> unused{};
    std::size_t size = 0;
    assert(inputStream.read(unused, 0ms, size, error) == asb::platform::HidReadStatus::Error);
    ::close(hostSockets[1]);
}

void uhidTests() {
    int sockets[2];
    assert(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0);
    UhidDevice pad(sockets[0]);
    std::string error;
    std::vector<std::string> trace;
    pad.setTraceHandler([&](std::string_view line) { trace.emplace_back(line); });
    const std::array<std::uint8_t, 6> address{1, 2, 3, 4, 5, 2};
    assert(pad.create(address, error));
    auto creation = receive(sockets[1]);
    assert(creation.type == UHID_CREATE2 && creation.u.create2.bus == BUS_USB);
    assert(creation.u.create2.vendor == 0x054c && creation.u.create2.product == 0x0ce6);
    assert(creation.u.create2.rd_size == 273);

    auto transport = std::make_unique<ApexTransport>();
    auto* usb = transport.get();
    asb::flydigi::Apex5Device apex(asb::flydigi::TransportPtr(transport.release()));
    assert(apex.verifyIdentity(error));
    asb::dualsense::AdaptiveTriggerBridge bridge(apex);
    const auto handler = [&](const auto& feedback) { bridge.handle(feedback); };
    uhid_event event{};
    event.type = UHID_START;
    sendEvent(sockets[1], event);
    assert(pad.dispatch(handler, error) && pad.started());

    // Linux driver initialization asks for these exact feature reports.
    for (unsigned id : {5U, 9U, 0x20U, 0xffU}) {
        event = {};
        event.type = UHID_GET_REPORT;
        event.u.get_report.id = 1234 + id;
        event.u.get_report.rnum = id;
        event.u.get_report.rtype = UHID_FEATURE_REPORT;
        sendEvent(sockets[1], event);
        assert(pad.dispatch(handler, error));
        const auto reply = receive(sockets[1]);
        assert(reply.type == UHID_GET_REPORT_REPLY && reply.u.get_report_reply.id == 1234 + id);
        if (id == 0xff) assert(reply.u.get_report_reply.err == EOPNOTSUPP);
        else {
            assert(reply.u.get_report_reply.err == 0);
            assert(reply.u.get_report_reply.data[0] == id);
            if (id == 9) assert(std::equal(address.begin(), address.end(), reply.u.get_report_reply.data + 1));
        }
    }

    // Overwatch's initialization repeatedly reads 0x09/0x20 and then writes
    // feature 0x08 (48 bytes). The user's trace showed this request rejected
    // 99 times; verify the setup can finish before trigger output is sent.
    event = {};
    event.type = UHID_SET_REPORT;
    event.u.set_report.id = 800;
    event.u.set_report.rtype = UHID_FEATURE_REPORT;
    event.u.set_report.rnum = 8;
    event.u.set_report.size = 48;
    event.u.set_report.data[0] = 8;
    event.u.set_report.data[1] = 0x11;
    sendEvent(sockets[1], event);
    assert(pad.dispatch(handler, error));
    auto setupReply = receive(sockets[1]);
    assert(setupReply.type == UHID_SET_REPORT_REPLY && setupReply.u.set_report_reply.id == 800);
    assert(setupReply.u.set_report_reply.err == 0);
    assert(usb->writes.empty() && pad.outputs() == 0); // Setup never reaches the physical controller.
    event = {};
    event.type = UHID_GET_REPORT;
    event.u.get_report.id = 801;
    event.u.get_report.rtype = UHID_FEATURE_REPORT;
    event.u.get_report.rnum = 8;
    sendEvent(sockets[1], event);
    assert(pad.dispatch(handler, error));
    setupReply = receive(sockets[1]);
    assert(setupReply.u.get_report_reply.err == 0 && setupReply.u.get_report_reply.size == 48);
    assert(setupReply.u.get_report_reply.data[0] == 8 && setupReply.u.get_report_reply.data[1] == 0x11);
    // A wrong report prefix, oversized write or unrelated feature stays rejected.
    for (unsigned invalid : {0U, 1U, 2U}) {
        event = {};
        event.type = UHID_SET_REPORT;
        event.u.set_report.id = 802 + invalid;
        event.u.set_report.rtype = UHID_FEATURE_REPORT;
        event.u.set_report.rnum = invalid == 2 ? 0xff : 8;
        event.u.set_report.size = invalid == 1 ? 49 : 48;
        event.u.set_report.data[0] = invalid == 0 ? 0xff : 8;
        sendEvent(sockets[1], event);
        assert(pad.dispatch(handler, error));
        assert(receive(sockets[1]).u.set_report_reply.err == EOPNOTSUPP);
    }

    // Game -> UHID_OUTPUT -> real translator -> Apex command 0x51.
    event = {};
    event.type = UHID_OUTPUT;
    event.u.output.rtype = UHID_OUTPUT_REPORT;
    event.u.output.size = 48;
    auto* data = event.u.output.data;
    data[0] = 2; data[1] = 0x0c;
    data[11] = 0x21; data[12] = 4; data[14] = 0x40; // Right: zone 2, strength 2
    data[22] = 0x21; data[23] = 8; data[26] = 2; // Left: zone 3, strength 2
    data[33] = 0x12; data[34] = 0x34;
    sendEvent(sockets[1], event);
    assert(pad.dispatch(handler, error) && !bridge.failed());
    assert(usb->writes.size() == 2 && bridge.stats().active == 2);
    for (const auto& packet : usb->writes) {
        assert(packet.size() == 32 && packet[0] == 3 && packet[3] == 0x51);
    }
    assert(pad.outputs() == 1);
    // Duplicate effects are coalesced by the shared translator.
    sendEvent(sockets[1], event);
    assert(pad.dispatch(handler, error) && usb->writes.size() == 2);
    asb::dualsense::DualSenseInputState state;
    state.l2 = 255; state.r2 = 255;
    assert(pad.update(state, error));
    const auto input = receive(sockets[1]);
    assert(input.type == UHID_INPUT2 && input.u.input2.size == 64);
    assert(input.u.input2.data[42] == 0x12 && input.u.input2.data[43] == 0x13);
    assert(input.u.input2.data[44] == 0x12 && input.u.input2.data[45] == 0x34);
    assert(input.u.input2.data[48] == 0x11);

    // Native HID consumers may request a snapshot instead of using ReadFile.
    // The initial Linux implementation incorrectly returned EOPNOTSUPP here.
    event = {};
    event.type = UHID_GET_REPORT;
    event.u.get_report.id = 700;
    event.u.get_report.rnum = 1;
    event.u.get_report.rtype = UHID_INPUT_REPORT;
    sendEvent(sockets[1], event);
    assert(pad.dispatch(handler, error));
    auto snapshot = receive(sockets[1]);
    assert(snapshot.type == UHID_GET_REPORT_REPLY && snapshot.u.get_report_reply.err == 0);
    assert(snapshot.u.get_report_reply.id == 700 && snapshot.u.get_report_reply.size == 64);
    assert(std::equal(input.u.input2.data, input.u.input2.data + 64, snapshot.u.get_report_reply.data));
    event.u.get_report.id = 701;
    event.u.get_report.rnum = 2;
    event.u.get_report.rtype = UHID_OUTPUT_REPORT;
    sendEvent(sockets[1], event);
    assert(pad.dispatch(handler, error));
    snapshot = receive(sockets[1]);
    assert(snapshot.u.get_report_reply.err == 0 && snapshot.u.get_report_reply.size == 48);
    assert(snapshot.u.get_report_reply.data[0] == 2 && snapshot.u.get_report_reply.data[11] == 0x21);
    event.u.get_report.rnum = 0xff;
    sendEvent(sockets[1], event);
    assert(pad.dispatch(handler, error));
    assert(receive(sockets[1]).u.get_report_reply.err == EOPNOTSUPP);
    assert(std::any_of(trace.begin(), trace.end(), [](const auto& line) {
        return line.find("GET_REPORT type=2 number=0x1 bytes=64 result=ok") != std::string::npos;
    }));

    // Control-channel SET_REPORT uses the same path and acknowledges its ID.
    event = {};
    event.type = UHID_SET_REPORT;
    event.u.set_report.id = 456;
    event.u.set_report.rtype = UHID_OUTPUT_REPORT;
    event.u.set_report.rnum = 2;
    event.u.set_report.size = 48;
    event.u.set_report.data[0] = 2; event.u.set_report.data[1] = 0x0c;
    event.u.set_report.data[11] = 5; event.u.set_report.data[22] = 5; // Off
    sendEvent(sockets[1], event);
    assert(pad.dispatch(handler, error));
    auto reply = receive(sockets[1]);
    assert(reply.type == UHID_SET_REPORT_REPLY && reply.u.set_report_reply.id == 456 && reply.u.set_report_reply.err == 0);
    assert(usb->writes.size() == 4 && bridge.stats().normal == 2);
    event.u.set_report.rtype = UHID_FEATURE_REPORT;
    sendEvent(sockets[1], event);
    assert(pad.dispatch(handler, error));
    assert(receive(sockets[1]).u.set_report_reply.err == EOPNOTSUPP);
    event.u.set_report.rtype = UHID_OUTPUT_REPORT;
    event.u.set_report.size = UHID_DATA_MAX + 1;
    sendEvent(sockets[1], event);
    assert(pad.dispatch(handler, error));
    assert(receive(sockets[1]).u.set_report_reply.err == EOPNOTSUPP);

    // A physical output failure is exposed, never silently marked successful.
    usb->fail = true;
    event.u.set_report.size = 48;
    event.u.set_report.data[11] = 0x21; event.u.set_report.data[12] = 4;
    sendEvent(sockets[1], event);
    assert(pad.dispatch(handler, error) && bridge.failed());
    (void)receive(sockets[1]);
    ::close(sockets[1]);
    assert(!pad.dispatch(handler, error));
}
} // namespace

int main() {
    descriptorTests();
    reportTests();
    rgbTests();
    reconnectSettingsTests();
    busyFeedbackInputTests();
    uhidTests();
    std::cout << "Linux HID/UHID bridge tests passed\n";
}
