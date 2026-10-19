#include "platform/linux/InputPump.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <poll.h>

namespace asb::platform::linux_backend {
using namespace std::chrono_literals;

InputPump::InputPump(InputStream& input, InputPipeline& pipeline, UhidDevice& pad,
                     const dualsense::DualSenseInputState& initial)
    : input_(&input), pipeline_(pipeline), pad_(pad), physical_(initial) {}

InputPump::InputPump(EvdevInput& input, InputPipeline& pipeline, UhidDevice& pad,
                     const dualsense::DualSenseInputState& initial)
    : evdev_(&input), pipeline_(pipeline), pad_(pad), physical_(initial) {}

bool InputPump::step(const UhidDevice::Handler& feedback, const Observer& observer, std::string& error) {
    // Either descriptor wakes the same wait immediately. Only quiet periods
    // use the 1 ms heartbeat timer (touch gestures and sensor/host timestamps).
    const auto remaining = evdev_ && evdev_->buffered() ? Clock::duration::zero()
        : std::max(Clock::duration::zero(), lastSent_ + 1ms - Clock::now());
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count();
    const timespec timeout{static_cast<time_t>(ns / 1000000000), static_cast<long>(ns % 1000000000)};
    const int inputFd = input_ ? input_->nativeHandle() : evdev_->nativeHandle();
    pollfd ready[]{{inputFd, POLLIN, 0}, {pad_.nativeHandle(), POLLIN, 0}};
    const auto result = ::ppoll(ready, 2, &timeout, nullptr);
    if (result < 0 && errno == EINTR) return true; // Return to the signal/stop check.
    if (result < 0) { error = "Cannot wait for controller input: " + std::string(std::strerror(errno)); return false; }
    for (const auto& entry : ready) {
        if (entry.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            error = entry.fd == inputFd ? "Apex input stream disconnected" : "UHID connection closed";
            return false;
        }
    }
    // Drain a bounded batch before servicing ONE feedback event. At high input
    // rates both directions progress; a color/output flood cannot delay buttons
    // behind a batch of 64 output callbacks. Metadata also counts toward budget.
    std::array<std::uint8_t, 4096> packet;
    for (unsigned i = 0; i < 32; ++i) {
        std::size_t size = 0;
        std::optional<dualsense::DualSenseInputState> state;
        const auto status = input_ ? input_->read(packet, 0ms, size, error) : evdev_->read(state, error);
        if (status == HidReadStatus::Error) return false;
        if (status != HidReadStatus::Data) break;
        if (input_) state = pipeline_.decode({packet.data(), size});
        if (state) {
            physical_ = *state;
            const auto now = Clock::now();
            lastInput_ = now;
            if (!pad_.update(pipeline_.render(physical_, now), error)) return false;
            lastSent_ = now;
            ++stats_.reports; ++stats_.forwarded;
            if (state->gyroX || state->gyroY || state->gyroZ) ++stats_.motion;
            // Forward buttons first, then queue any physical bow/trigger update.
            if (observer) observer(*state);
        }
    }
    if ((ready[1].revents & POLLIN) && !pad_.dispatch(feedback, error, 1)) return false;
    const auto now = Clock::now();
    // Evdev is event-driven: an idle Apex 6 is healthy without repeated reports.
    // poll/read still catch removal. Raw Apex 4/5 streams must stay continuous.
    if (!evdev_ && now - lastInput_ >= 1500ms) {
        error = "Apex input stopped; ending the session and clearing triggers. Wake it and rerun.";
        return false;
    }
    if (!pad_.started()) { error = "Virtual DualSense stopped unexpectedly."; return false; }
    if (now - lastSent_ >= 1ms) {
        if (!pad_.update(pipeline_.render(physical_, now), error)) return false;
        lastSent_ = now;
        ++stats_.heartbeats;
    }
    return true;
}

} // namespace asb::platform::linux_backend
