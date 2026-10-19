#ifdef NDEBUG
#undef NDEBUG
#endif
#include "platform/linux/AsyncDiagnostics.h"
#include "platform/linux/InputPump.h"

#include <algorithm>
#include <cassert>
#include <future>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace {
using namespace asb::platform::linux_backend;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

struct Pair {
    int fd[2];
    Pair() { assert(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fd) == 0); }
    ~Pair() { for (const auto value : fd) if (value >= 0) ::close(value); }
    int take() { return std::exchange(fd[0], -1); }
};

asb::flydigi::Apex5Identity identity() {
    const auto id = asb::flydigi::Apex5Identity::parseReply(std::array<std::uint8_t,32>{4,0x5a,0xa5,1,0,0,128,1});
    assert(id); return *id;
}

std::array<std::uint8_t,32> packet(unsigned index) {
    std::array<std::uint8_t,32> raw{4,0x5a,0xa5,0xef};
    raw[4] = index * 17; raw[5] = index * 5;
    raw[8] = index * 13; raw[9] = index * 3;
    raw[12] = index & 1 ? 0x10 : 0; // Alternate press/release with no debounce.
    raw[13] = index & 1 ? 0xce : 0; // Menu, shoulders and stick clicks.
    raw[15] = index & 1 ? 8 : 0;
    raw[16] = index; raw[17] = index >> 8; // Also a unique host-visible sample ID.
    raw[18] = index; raw[19] = 0xff;
    return raw;
}

struct Fixture {
    Pair physical, host;
    InputStream input{physical.take()};
    UhidDevice pad{host.take()};
    InputPipeline pipeline;
    InputPump pump;
    std::string error;
    Fixture(Options options = {}) : pipeline(identity(), options), pump(input, pipeline, pad, {}) {
        uhid_event start{}; start.type = UHID_START;
        assert(::send(host.fd[1], &start, sizeof(start), 0) == sizeof(start));
        assert(pad.dispatch([](const auto&) {}, error) && pad.started());
    }
    void push(std::span<const std::uint8_t> raw) {
        assert(::send(physical.fd[1], raw.data(), raw.size(), 0) == static_cast<ssize_t>(raw.size()));
    }
    std::vector<std::array<std::uint8_t,64>> drain() {
        std::vector<std::array<std::uint8_t,64>> result;
        for (;;) {
            uhid_event event{};
            const auto count = ::recv(host.fd[1], &event, sizeof(event), MSG_DONTWAIT);
            if (count < 0) { assert(errno == EAGAIN); break; }
            assert(event.type == UHID_INPUT2 && event.u.input2.size == 64);
            assert(count == static_cast<ssize_t>(offsetof(uhid_event, u.input2.data) + 64));
            std::array<std::uint8_t,64> data;
            std::copy_n(event.u.input2.data, 64, data.begin());
            result.push_back(data);
        }
        return result;
    }
    void output(unsigned count) {
        uhid_event event{}; event.type = UHID_OUTPUT;
        event.u.output.size = 48; event.u.output.rtype = UHID_OUTPUT_REPORT;
        event.u.output.data[0] = 2; event.u.output.data[2] = 4;
        event.u.output.data[45] = 80;
        for (unsigned i = 0; i < count; ++i) assert(::send(host.fd[1], &event, sizeof(event), 0) == sizeof(event));
    }
};

void burstAndPriority() {
    Fixture f;
    f.output(12);
    for (unsigned i = 1; i <= 64; ++i) f.push(packet(i));
    unsigned forwarded = 0, callbacks = 0;
    const auto observe = [&](const auto& state) {
        const auto received = f.drain();
        // The actual UHID packet must already be sent before trigger work runs.
        assert(received.size() == 1);
        const auto& data = received.front();
        ++forwarded;
        const auto expected = buildUsbInput(state, 0, 0);
        for (unsigned j = 0; j < 28; ++j) if (j != 7) assert(data[j] == expected[j]);
        assert((unsigned(data[5]) | unsigned(data[6]) << 8) == forwarded);
        assert(bool(data[8] & 0x20) == bool(forwarded & 1));
    };
    const auto feedback = [&](const auto&) {
        ++callbacks;
        assert(forwarded >= std::min(64U, 32 * callbacks));
    };
    assert(f.pump.step(feedback, observe, f.error));
    assert(forwarded == 32 && callbacks == 1); (void)f.drain();
    assert(f.pump.step(feedback, observe, f.error));
    assert(forwarded == 64 && callbacks == 2); (void)f.drain();
    for (unsigned i = 2; i < 12; ++i) { assert(f.pump.step(feedback, observe, f.error)); (void)f.drain(); }
    assert(callbacks == 12 && f.pad.outputs() == 12);
    assert(f.pump.stats().reports == 64 && f.pump.stats().forwarded == 64);
}

void blockedDiagnostics() {
    Fixture f;
    AsyncDiagnostics diagnostics;
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    while (!diagnostics.submit([&] { entered.set_value(); released.wait(); })) {}
    entered.get_future().wait();
    std::atomic_uint completed{0};
    unsigned accepted = 0;
    for (unsigned i = 0; i < 512; ++i) if (diagnostics.submit([&] { ++completed; })) ++accepted;
    assert(accepted <= 256 && diagnostics.dropped() >= 256);
    unsigned observed = 0;
    for (unsigned batch = 0; batch < 8; ++batch) {
        for (unsigned i = 1; i <= 32; ++i) f.push(packet(batch * 32 + i));
        assert(f.pump.step([](const auto&) {}, [&](const auto&) { ++observed; }, f.error));
        const auto reports = f.drain();
        assert(reports.size() >= 32);
    }
    // Input progresses even while the output worker and its full queue cannot.
    assert(observed == 256 && completed == 0);
    release.set_value(); diagnostics.finish();
    assert(completed == accepted && !diagnostics.failed());

    AsyncDiagnostics failure;
    while (!failure.submit([] { throw std::runtime_error("disk full"); })) {}
    failure.finish();
    assert(failure.failed() && failure.error() == "disk full");
}

void heartbeatAndDisconnect() {
    Options options; options.touchpadProfile = asb::dualsense::TouchpadGestureProfile::LegacyViewHoldSwipeUp;
    Fixture f(options);
    auto raw = packet(0); raw[12] = 0x40; // Tap View, then release.
    f.push(raw); assert(f.pump.step([](const auto&) {}, {}, f.error)); (void)f.drain();
    raw[12] = 0; f.push(raw); assert(f.pump.step([](const auto&) {}, {}, f.error));
    auto reports = f.drain(); assert(!reports.empty() && (reports.front()[10] & 2));
    bool released = false;
    const auto deadline = Clock::now() + 100ms;
    while (Clock::now() < deadline && !released) {
        assert(f.pump.step([](const auto&) {}, {}, f.error));
        for (const auto& report : f.drain()) if (!(report[10] & 2)) released = true;
    }
    assert(released && f.pump.stats().reports == 2 && f.pump.stats().heartbeats > 0);
    ::close(f.physical.fd[1]); f.physical.fd[1] = -1;
    assert(!f.pump.step([](const auto&) {}, {}, f.error));
    assert(f.error.find("disconnected") != std::string::npos);
}

// This measures local enqueue-to-receive software latency, not USB/radio,
// Proton, game polling, display latency or physical button-to-photon time.
void benchmark(bool loaded) {
    constexpr unsigned count = 2000;
    Fixture f;
    AsyncDiagnostics diagnostics;
    std::vector<Clock::time_point> sent(count + 1);
    std::atomic_uint published{0};
    std::thread source([&] {
        auto next = Clock::now();
        for (unsigned i = 1; i <= count; ++i) {
            next += 500us; std::this_thread::sleep_until(next);
            const auto raw = packet(i);
            sent[i] = Clock::now();
            published.store(i, std::memory_order_release);
            while (::send(f.physical.fd[1], raw.data(), raw.size(), 0) < 0) {
                assert(errno == EAGAIN); std::this_thread::yield();
            }
        }
    });
    unsigned received = 0;
    std::vector<double> latency;
    while (received < count) {
        if (loaded) {
            f.output(1);
            diagnostics.submit([] { std::this_thread::sleep_for(2ms); });
        }
        assert(f.pump.step([](const auto&) {}, {}, f.error));
        for (const auto& report : f.drain()) {
            const auto id = unsigned(report[5]) | unsigned(report[6]) << 8;
            if (id <= received) continue; // Synthetic heartbeat of the last state.
            assert(id == received + 1 && published.load(std::memory_order_acquire) >= id);
            assert(bool(report[8] & 0x20) == bool(id & 1));
            latency.push_back(std::chrono::duration<double, std::micro>(Clock::now() - sent[id]).count());
            ++received;
        }
    }
    source.join();
    const auto outputs = f.pad.outputs();
    const auto drops = diagnostics.dropped();
    diagnostics.finish();
    std::sort(latency.begin(), latency.end());
    const auto percentile = [&](double fraction) { return latency[static_cast<std::size_t>((latency.size() - 1) * fraction)]; };
    std::cout << "{\"scenario\":\"" << (loaded ? "feedback_and_slow_diagnostics" : "input_only")
              << "\",\"samples\":" << count << ",\"injection_hz\":2000,\"missing_or_reordered\":0"
              << ",\"median_us\":" << percentile(0.5) << ",\"p95_us\":" << percentile(0.95)
              << ",\"p99_us\":" << percentile(0.99) << ",\"maximum_us\":" << latency.back()
              << ",\"feedback_events\":" << outputs << ",\"diagnostic_drops\":" << drops << "}\n";
}
} // namespace

int main(int argc, char**) {
    if (argc > 1) { benchmark(false); benchmark(true); return 0; }
    burstAndPriority(); blockedDiagnostics(); heartbeatAndDisconnect();
    std::cout << "Immediate ordered inputs, feedback fairness, bounded diagnostics and gesture heartbeats passed\n";
}
