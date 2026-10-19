#include "platform/linux/EvdevRumble.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace asb::platform::linux_backend {
using namespace std::chrono_literals;

EvdevRumble::EvdevRumble(int fd, unsigned strength) : strength_(strength) {
    fd_ = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (fd_ < 0) { error_ = "Cannot retain the rumble device"; failed_ = true; return; }
    writer_ = [this](auto left, auto right, auto& error) { return write(left, right, error); };
    worker_ = std::thread([this] { run(); });
}
EvdevRumble::EvdevRumble(Writer writer, unsigned strength) : writer_(std::move(writer)), strength_(strength), worker_([this] { run(); }) {}
EvdevRumble::~EvdevRumble() {
    std::string ignored; (void)stop(ignored);
    if (effect_ >= 0) (void)::ioctl(fd_, EVIOCRMFF, effect_);
    if (fd_ >= 0) ::close(fd_);
}
void EvdevRumble::handle(const dualsense::DualSenseFeedback& feedback) {
    if (feedback.requestsRumbleUpdate()) set(feedback.rumbleLeft, feedback.rumbleRight);
}
void EvdevRumble::set(std::uint8_t left, std::uint8_t right) {
    const auto scale = [this](unsigned value) { return static_cast<std::uint16_t>(std::min(255U, value * strength_ / 100) * 257); };
    { std::lock_guard lock(mutex_); if (stopping_) return; left_ = scale(left); right_ = scale(right); pending_ = true; ++updates_; }
    cv_.notify_one();
}
std::string EvdevRumble::error() const { std::lock_guard lock(mutex_); return error_; }

bool EvdevRumble::write(std::uint16_t left, std::uint16_t right, std::string& error) {
    if (left || right) {
        ff_effect effect{};
        effect.type = FF_RUMBLE; effect.id = static_cast<std::int16_t>(effect_);
        effect.u.rumble.strong_magnitude = left; effect.u.rumble.weak_magnitude = right;
        effect.replay.length = 1000;
        if (::ioctl(fd_, EVIOCSFF, &effect) < 0) { error = "Cannot upload grip rumble: " + std::string(std::strerror(errno)); return false; }
        effect_ = effect.id;
    }
    if (effect_ < 0) return true;
    input_event event{}; event.type = EV_FF; event.code = static_cast<std::uint16_t>(effect_); event.value = (left || right) ? 1 : 0;
    ssize_t bytes;
    do { bytes = ::write(fd_, &event, sizeof(event)); } while (bytes < 0 && errno == EINTR);
    if (bytes != sizeof(event)) { error = "Cannot play/stop grip rumble: " + std::string(std::strerror(errno)); return false; }
    return true;
}
void EvdevRumble::run() {
    bool active = false;
    for (;;) {
        std::unique_lock lock(mutex_);
        if (active) cv_.wait_for(lock, 250ms, [this] { return stopping_ || pending_; });
        else cv_.wait(lock, [this] { return stopping_ || pending_; });
        if (stopping_) return;
        const auto left = left_, right = right_; pending_ = false; active = left || right;
        lock.unlock();
        std::string error;
        bool ok = false;
        try { ok = writer_(left, right, error); }
        catch (...) { error = "Grip rumble worker failed"; }
        ++writes_;
        if (!ok) { std::lock_guard guard(mutex_); error_ = std::move(error); failed_.store(true, std::memory_order_release); return; }
    }
}
bool EvdevRumble::stop(std::string& error) {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (stopped_) { if (failed()) error = this->error(); return !failed(); }
    stopped_ = true;
    try { if (writer_ && !writer_(0, 0, error)) return false; }
    catch (...) { error = "Cannot stop grip rumble"; return false; }
    if (failed()) error = this->error();
    return !failed();
}

} // namespace asb::platform::linux_backend
