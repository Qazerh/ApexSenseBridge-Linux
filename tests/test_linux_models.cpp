#ifdef NDEBUG
#undef NDEBUG
#endif
#include "platform/linux/FeedbackBridge.h"
#include "platform/linux/HidReportDescriptor.h"
#include "platform/linux/InputPump.h"
#include "flydigi/Apex4Protocol.h"

#include <cassert>
#include <deque>
#include <future>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

namespace {
using namespace asb;
using namespace asb::platform::linux_backend;
using namespace std::chrono_literals;

class Controller final : public platform::HidTransport {
public:
    explicit Controller(unsigned model, std::uint8_t type = 0x96) : model(model), type(type) {
        info_.vendorId = model == 4 ? 0x04b4 : 0x37d7;
        info_.productId = model == 4 ? 0x2412 : model == 5 ? 0x2501 : 0x2502;
        info_.usagePage = 0xffa0; info_.interfaceNumber = model == 4 ? L"MI_02" : L"MI_01";
        info_.inputReportLength = 32; info_.outputReportLength = model == 6 ? 33 : 64;
    }
    bool isOpen() const noexcept override { return true; }
    const HidDeviceInfo& info() const noexcept override { return info_; }
    bool writeOutputReport(std::span<const std::uint8_t> data, std::string& error) override {
        std::lock_guard lock(mutex_);
        if (fail && (model != 6 || data[3] == 0x57)) { error = "simulated lost controller"; return false; }
        writes_.emplace_back(data.begin(), data.end());
        if (model == 4 && data[0] == 5 && data[1] == 0xec) {
            std::vector<std::uint8_t> reply(32); reply[3] = 84; reply[13] = 1; reply[15] = 0xec;
            replies_.push_back(reply);
        } else if (model == 5 && data[3] == 1) {
            std::vector<std::uint8_t> reply(32); reply[0]=4; reply[1]=0x5a; reply[2]=0xa5; reply[3]=1;
            reply[6]=134; reply[7]=2; replies_.push_back(reply);
        } else if (model == 6 && data[3] == 1) {
            // Linux unnumbered input has no leading zero report-ID byte.
            std::vector<std::uint8_t> reply(32); reply[0]=0x5a; reply[1]=0xa5; reply[2]=1; reply[3]=1;
            reply[5]=type; reply[6]=0; reply[29]=0x9f;
            reply[31]=flydigi::apex6::checksum(reply); replies_.push_back(reply);
        }
        return true;
    }
    platform::HidReadStatus readInputReport(std::span<std::uint8_t> data, std::chrono::milliseconds,
                                          std::size_t& size, std::string&) override {
        std::lock_guard lock(mutex_); size=0;
        if (replies_.empty()) return platform::HidReadStatus::Timeout;
        std::copy(replies_.front().begin(), replies_.front().end(), data.begin());
        size=replies_.front().size(); replies_.pop_front(); return platform::HidReadStatus::Data;
    }
    std::vector<std::vector<std::uint8_t>> writes() { std::lock_guard lock(mutex_); return writes_; }
    unsigned model;
    std::uint8_t type;
    std::atomic_bool fail{false};
private:
    HidDeviceInfo info_;
    std::mutex mutex_;
    std::deque<std::vector<std::uint8_t>> replies_;
    std::vector<std::vector<std::uint8_t>> writes_;
};

void models() {
    for (const auto model : {4U, 5U, 6U}) {
        auto* wire = new Controller(model);
        flydigi::Apex5Device device{flydigi::TransportPtr(wire)};
        std::string error;
        assert(device.verifyIdentity(error, {}, true));
        Options options;
        FeedbackBridge feedback(device, options);
        feedback.setLogHandler([](auto) {});
        assert(feedback.start(error));
        dualsense::DualSenseFeedback command;
        command.enableBits1 = 15; command.rumbleLeft = 80; command.rumbleRight = 50;
        command.leftTriggerEffect = {0x21,0xfc,3}; command.rightTriggerEffect = command.leftTriggerEffect;
        feedback.updateTriggerPositions(255, 255);
        feedback.handle(command);
        std::this_thread::sleep_for(90ms);
        assert(!feedback.failed());
        assert(feedback.triggerStats().translated >= 2 && feedback.rumbleStats().updates >= 1);
        if (model == 6) assert(feedback.hapticFrames() > 0);
        assert(feedback.stop(error));
        const auto writes = wire->writes();
        assert(writes.size() > 3);
        for (const auto& data : writes) {
            if (model == 4) assert(data[0] == 5 && (data[1] == 0xec || data[1] == 0xa0 || data[1] == 0x0f));
            if (model == 5) assert(data[0] == 3 && data[1] == 0x5a && data[2] == 0xa5);
            if (model == 6) {
                assert(data.size() == 33 && data[0] == 0 && data[1] == 0x5a && data[2] == 0xa5);
                assert(data[3] == 1 || data[3] == 0x53 || data[3] == 0x57);
                assert(data[32] == flydigi::apex6::checksum(std::span(data).subspan(1)));
            }
        }
    }
    for (const auto type : {0x95,0x98,0x99}) {
        auto* wire = new Controller(6, type);
        flydigi::Apex5Device device{flydigi::TransportPtr(wire)};
        std::string error;
        const bool verified = device.verifyIdentity(error, {}, true);
        assert(verified == (type != 0x99));
        if (type == 0x95) {
            assert(!device.identity()->supportsRealtimeHaptics()); // Even with misleading 0x9F flags.
            assert(!device.enableApex6Haptics(error));
            assert(!device.clearAll(error));
            FeedbackBridge feedback(device, {});
            dualsense::DualSenseFeedback command; command.enableBits1 = 15;
            feedback.handle(command); feedback.updateTriggerPositions(255,255);
            assert(feedback.start(error) && feedback.stop(error));
            assert(!feedback.diagnosticTrigger({}, error));
            assert(wire->writes().size() == 1); // Only the read-only identity query.
        }
    }
}

void apex4Input() {
    auto* wire = new Controller(4);
    flydigi::Apex5Device device{flydigi::TransportPtr(wire)};
    std::string error; assert(device.verifyIdentity(error));
    Options options; options.gyroStrength=200; options.gyroYaw=50;
    InputPipeline pipeline(*device.identity(), options);
    std::array<std::uint8_t,32> raw{4,0xfe};
    raw[9]=0x10; raw[10]=0x86; raw[17]=127; raw[19]=0; raw[21]=255; raw[22]=127;
    raw[23]=123; raw[24]=234; raw[26]=10; raw[18]=20; raw[29]=5;
    const auto input=pipeline.decode(raw); assert(input);
    const auto report=buildUsbInput(*input,0,0);
    assert(report[1]==128 && report[2]==0 && report[3]==255 && report[4]==128);
    assert(report[5]==123 && report[6]==234 && (report[8]&0x20) && (report[9]&0x20));
    assert(input->gyroX==180 && input->gyroY==100 && input->gyroZ==-24);
    raw[1]=0x5a; raw[2]=0xa5; raw[3]=0xef;
    assert(!pipeline.decode(raw));
}

void diagnosticGain() {
    for (const auto model : {4U,5U}) {
        auto* wire=new Controller(model);
        flydigi::Apex5Device device{flydigi::TransportPtr(wire)};
        std::string error; assert(device.verifyIdentity(error));
        Options options; options.strength=50; options.rumbleStrength=50;
        FeedbackBridge feedback(device,options);
        ForceTriggerCommand trigger; trigger.mode=TriggerMode::Race; trigger.params={60,80,0,0,0};
        assert(feedback.diagnosticTrigger(trigger,error));
        assert(feedback.diagnosticGrips(80,40,error));
        const auto reports=wire->writes();
        assert(reports[1][model==4 ? 7 : 9]==40); // Half force, unchanged position.
        assert(reports[2][model==4 ? 2 : 5]==40 && reports[2][model==4 ? 3 : 6]==20);
        assert(feedback.stop(error));
    }
}

EvdevLayout layout(bool generic=false) {
    EvdevLayout l; l.hidTriggerOrder=generic; l.rawHidButtons=generic;
    for (const auto axis : {ABS_X,ABS_Y,ABS_RX,ABS_RY}) { l.present[axis]=true; l.axes[axis].minimum=-32768; l.axes[axis].maximum=32767; }
    for (const auto axis : {ABS_Z,ABS_RZ}) { l.present[axis]=true; l.axes[axis].maximum=1023; }
    l.present[ABS_HAT0X]=l.present[ABS_HAT0Y]=true;
    return l;
}
input_event event(unsigned type, unsigned code, int value) {
    input_event e{}; e.type=type; e.code=code; e.value=value; return e;
}
void mappedInput() {
    for (const bool generic : {false,true}) {
        int sockets[2]; assert(::socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0,sockets)==0);
        EvdevInput source(sockets[0],layout(generic));
        const std::array events{
            event(EV_ABS,ABS_Z,1023),event(EV_ABS,ABS_RZ,512),event(EV_ABS,ABS_HAT0X,-1),
            event(EV_KEY,generic ? BTN_GAMEPAD+2 : BTN_X,1),event(EV_KEY,generic ? BTN_GAMEPAD+3 : BTN_Y,1),
            event(EV_SYN,SYN_REPORT,0),
            event(EV_KEY,generic ? BTN_GAMEPAD+2 : BTN_X,0),event(EV_KEY,generic ? BTN_GAMEPAD+3 : BTN_Y,0),
            event(EV_SYN,SYN_REPORT,0)};
        assert(::send(sockets[1],events.data(),sizeof(events),0)==sizeof(events));
        std::optional<dualsense::DualSenseInputState> state; std::string error;
        assert(source.read(state,error)==platform::HidReadStatus::Data && state);
        assert(state->lx==128 && state->ly==128 && state->dpad==4);
        assert(state->l2==(generic?128:255) && state->r2==(generic?255:128));
        assert((state->buttons&(dualsense::button::kSquare|dualsense::button::kTriangle))==(dualsense::button::kSquare|dualsense::button::kTriangle));
        assert(source.buffered());
        assert(source.read(state,error)==platform::HidReadStatus::Data && state);
        assert(!(state->buttons&(dualsense::button::kSquare|dualsense::button::kTriangle)));
        ::close(sockets[1]); assert(source.read(state,error)==platform::HidReadStatus::Error);
    }
}

void pump6() {
    int physical[2],host[2];
    assert(::socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0,physical)==0);
    assert(::socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0,host)==0);
    EvdevInput source(physical[0],layout()); UhidDevice pad(host[0]);
    uhid_event start{}; start.type=UHID_START; assert(::send(host[1],&start,sizeof(start),0)==sizeof(start));
    std::string error; assert(pad.dispatch([](const auto&){},error));
    auto* wire=new Controller(6); flydigi::Apex5Device device{flydigi::TransportPtr(wire)}; assert(device.verifyIdentity(error));
    InputPipeline pipeline(*device.identity(),{}); InputPump pump(source,pipeline,pad,source.snapshot());
    std::array<input_event,120> events;
    for (unsigned i=0;i<40;++i) {
        events[i*3]=event(EV_KEY,BTN_A,i%2);
        events[i*3+1]=event(EV_MSC,MSC_SCAN,0);
        events[i*3+2]=event(EV_SYN,SYN_REPORT,0);
    }
    // Two writes allow read() to fill its 64-event local buffer; the remainder
    // must be drained next iteration even if the kernel fd is no longer ready.
    assert(::send(physical[1],events.data(),64*sizeof(input_event),0)==64*sizeof(input_event));
    assert(::send(physical[1],events.data()+64,56*sizeof(input_event),0)==56*sizeof(input_event));
    unsigned received=0;
    auto drain=[&]{
        uhid_event e{};
        while (::recv(host[1],&e,sizeof(e),MSG_DONTWAIT)>0) {
            const bool down=e.u.input2.data[8]&0x20;
            if (received && down==bool((received-1)%2)) continue; // Quiet-period heartbeat.
            if (received<40) { assert(down==bool(received%2)); ++received; }
        }
    };
    assert(pump.step([](const auto&){},{},error)); drain();
    assert(pump.step([](const auto&){},{},error)); drain();
    assert(received==40 && pump.stats().forwarded==40);
    const auto until=std::chrono::steady_clock::now()+1600ms;
    while (std::chrono::steady_clock::now()<until) { assert(pump.step([](const auto&){},{},error)); drain(); }
    assert(pump.stats().forwarded==40 && pump.stats().heartbeats>0); // An idle evdev pad is healthy.
    ::close(physical[1]); assert(!pump.step([](const auto&){},{},error)); ::close(host[1]);
}

void kernelRumble() {
    std::promise<void> entered,release;
    auto released=release.get_future().share();
    std::vector<std::pair<unsigned,unsigned>> calls;
    EvdevRumble rumble([&](auto left,auto right,auto&){ calls.emplace_back(left,right); if(calls.size()==1) { entered.set_value(); released.wait(); } return true; },200);
    rumble.set(80,200); entered.get_future().wait();
    rumble.set(1,2); rumble.set(0,0); // A blocked FF driver cannot block/coalesce input.
    release.set_value(); std::string error; assert(rumble.stop(error));
    assert(calls.front()==std::make_pair(160U*257,65535U) && calls.back()==std::make_pair(0U,0U));
}

void descriptors() {
    const std::array<std::uint8_t,19> unnumbered{6,0xa0,0xff,9,1,0xa1,1,0x75,8,0x95,32,0x81,2,0x91,2,0xb1,2,0xc0,0};
    // Remove the final padding byte (not a descriptor item).
    const auto l=parseHidReportDescriptor(std::span(unnumbered).first(18));
    assert(l && !l->numbered && l->inputLength==32 && l->outputLength==32);
    assert(l->outputBufferLength()==33 && l->featureBufferLength()==33);
}
}
int main() {
    models(); apex4Input(); diagnosticGain(); mappedInput(); pump6(); kernelRumble(); descriptors();
    std::cout << "Apex 4/5/6/Pro identity guards, model feedback, V1 motion, evdev edges/idle/trigger axes and kernel rumble passed\n";
}
