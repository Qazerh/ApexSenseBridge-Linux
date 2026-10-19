#ifdef NDEBUG
#undef NDEBUG
#endif
#include "platform/linux/BridgeLifecycle.h"
#include "platform/linux/InputPump.h"

#include <cassert>
#include <iostream>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <utility>

namespace {
using namespace asb::platform::linux_backend;
using namespace std::chrono_literals;
struct Pair {
    int fd[2];
    Pair() { assert(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fd) == 0); }
    ~Pair() { for (int f : fd) if (f >= 0) ::close(f); }
    int take() { return std::exchange(fd[0], -1); }
};
asb::HidDeviceInfo device(std::wstring path, std::wstring location, std::wstring serial = {}) {
    asb::HidDeviceInfo d; d.path = path; d.containerId = location; d.serial = serial;
    d.vendorId = 0x37d7; d.productId = 0x2501; d.interfaceNumber = L"2"; return d;
}
void selection() {
    auto first = device(L"/dev/hidraw3", L"usb-1"), other = device(L"/dev/hidraw4", L"usb-2");
    ControllerSelector automatic("");
    assert(!automatic.select({}));
    assert(automatic.select({first})->path == first.path);
    bool ambiguous = false;
    try { (void)automatic.select({first, other}); } catch (const std::runtime_error&) { ambiguous = true; }
    assert(ambiguous);
    ControllerSelector selected("/dev/hidraw3");
    assert(!selected.select({other}));
    selected.remember(first, 128);
    auto replug = first; replug.path = L"/dev/hidraw9";
    other.path = first.path; // Old number reused by a different controller.
    assert(selected.select({other, replug})->path == replug.path);
    assert(!selected.select({other}));
    auto serial = device(L"/dev/hidraw5", L"usb-3", L"unique-serial");
    ControllerSelector bySerial(""); bySerial.remember(serial, 134);
    serial.path = L"/dev/hidraw8"; serial.containerId = L"usb-4";
    assert(bySerial.select({serial})->path == serial.path);
    auto wrongInterface = serial; wrongInterface.interfaceNumber = L"3";
    assert(!bySerial.select({wrongInterface}));
    bool changedModel = false;
    try { bySerial.remember(serial, 96); } catch (const std::runtime_error&) { changedModel = true; }
    assert(changedModel);
}
void feedbackCache() {
    FeedbackState cache;
    asb::dualsense::DualSenseFeedback f;
    f.enableBits1 = 4 | 1; f.rightTriggerEffect[0] = 0x21; f.rumbleLeft = 200;
    cache.remember(f);
    f = {}; f.enableBits1 = 8; f.leftTriggerEffect[0] = 0x25; cache.remember(f);
    f = {}; f.hasLightbar = true; f.lightbarRed = 50; cache.remember(f);
    auto saved = cache.snapshot();
    assert(saved.enableBits1 == 12 && saved.rumbleLeft == 0 && saved.rumbleRight == 0);
    assert(saved.leftTriggerEffect[0] == 0x25 && saved.rightTriggerEffect[0] == 0x21);
    assert(saved.hasLightbarColor() && saved.lightbarRed == 50);
    f.lightbarRed = 0; cache.remember(f); // Black must not be mistaken for missing.
    f = {}; f.enableBits1 = 4; cache.remember(f); // Explicit off replaces only right.
    saved = cache.snapshot();
    assert(saved.lightbarRed == 0 && saved.hasLightbarColor());
    assert(saved.rightTriggerEffect[0] == 0 && saved.leftTriggerEffect[0] == 0x25);
    f.kind = asb::dualsense::FeedbackKind::AudioHaptics; f.enableBits1 = 8; cache.remember(f);
    assert(cache.snapshot().leftTriggerEffect[0] == 0x25);
}
void sendEvent(int fd, const uhid_event& e) { assert(::send(fd, &e, sizeof(e), 0) == sizeof(e)); }
uhid_event receive(int fd) {
    pollfd ready{fd, POLLIN, 0}; assert(::poll(&ready, 1, 1000) == 1);
    uhid_event e{}; assert(::recv(fd, &e, sizeof(e), 0) >= 4); return e;
}
void persistentPad() {
    Pair host;
    UhidDevice pad(host.take());
    std::string error;
    assert(pad.create({1,2,3,4,5,2}, error));
    assert(receive(host.fd[1]).type == UHID_CREATE2);
    uhid_event e{}; e.type = UHID_START; sendEvent(host.fd[1], e);
    assert(pad.dispatch([](const auto&) {}, error) && pad.started());
    const int handle = pad.nativeHandle();
    FeedbackState cache;
    IdlePad idle(pad, cache);
    auto id = asb::flydigi::Apex5Identity::parseReply(std::array<std::uint8_t,32>{4,0x5a,0xa5,1,0,0,128,1});
    assert(id);
    const auto active = [&](bool unplug) {
        Pair physical;
        InputStream input(physical.take());
        InputPipeline pipeline(*id, {});
        InputPump pump(input, pipeline, pad, {});
        std::array<std::uint8_t,32> raw{4,0x5a,0xa5,0xef}; raw[12] = 0x10;
        assert(::send(physical.fd[1], raw.data(), raw.size(), 0) == static_cast<ssize_t>(raw.size()));
        assert(pump.step([](const auto&) {}, {}, error));
        auto report = receive(host.fd[1]);
        assert(report.type == UHID_INPUT2 && (report.u.input2.data[8] & 0x20));
        if (unplug) {
            ::close(physical.fd[1]); physical.fd[1] = -1;
            assert(!pump.step([](const auto&) {}, {}, error) && pump.physicalFailure());
        }
    };
    active(true);
    idle.start();
    e = {}; e.type = UHID_OUTPUT; e.u.output.rtype = UHID_OUTPUT_REPORT; e.u.output.size = 48;
    e.u.output.data[0] = 2; e.u.output.data[2] = 4; e.u.output.data[45] = 99; sendEvent(host.fd[1], e);
    e = {}; e.type = UHID_GET_REPORT; e.u.get_report.id = 7;
    e.u.get_report.rtype = UHID_FEATURE_REPORT; e.u.get_report.rnum = 8; sendEvent(host.fd[1], e);
    bool replied = false, neutral = false;
    for (unsigned i = 0; i < 30 && !(replied && neutral); ++i) {
        e = receive(host.fd[1]);
        if (e.type == UHID_INPUT2) {
            const auto* p = e.u.input2.data;
            neutral = p[0] == 1 && p[1] == 128 && p[2] == 128 && p[3] == 128 && p[4] == 128 &&
                p[5] == 0 && p[6] == 0 && p[8] == 8 && p[9] == 0 && p[10] == 0;
        } else {
            assert(e.type == UHID_GET_REPORT_REPLY);
            replied = e.u.get_report_reply.id == 7 && e.u.get_report_reply.err == 0;
        }
    }
    idle.stop(); assert(replied && neutral && !idle.failed());
    assert(cache.snapshot().lightbarRed == 99 && pad.started() && pad.nativeHandle() == handle);
    while (::recv(host.fd[1], &e, sizeof(e), MSG_DONTWAIT) >= 0) assert(e.type == UHID_INPUT2);
    active(false); // New physical fd, same virtual fd, no second UHID_CREATE.
    e = {}; e.type = UHID_STOP; sendEvent(host.fd[1], e);
    idle.start();
    for (unsigned i = 0; i < 100 && !idle.failed(); ++i) std::this_thread::sleep_for(2ms);
    idle.stop(); assert(idle.failed() && !idle.error().empty());
}
}
int main() {
    selection(); feedbackCache(); persistentPad();
    std::cout << "Controller identity, feedback cache and persistent UHID reconnect tests passed\n";
}
