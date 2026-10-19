#include "platform/linux/EvdevGrab.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace asb::platform::linux_backend {
EvdevGrab::~EvdevGrab() {
    for (const auto fd : fds_) ::close(fd); // Kernel releases grabs, including on a crash.
}

bool EvdevGrab::acquire(const HidDeviceInfo& device, std::string& error, const std::string& alreadyGrabbed) {
    namespace fs = std::filesystem;
    if (device.containerId.empty()) { error = "Cannot identify the controller's USB parent"; return false; }
    const fs::path parent(device.containerId);
    std::error_code ec;
    fs::directory_iterator entries("/sys/class/input", ec);
    if (ec) { error = ec.message(); return false; }
    for (const auto& entry : entries) {
        if (!entry.path().filename().string().starts_with("event")) continue;
        auto physical = fs::canonical(entry.path() / "device", ec);
        if (ec) { ec.clear(); continue; }
        bool matches = false;
        for (auto p = physical; p.has_relative_path(); p = p.parent_path()) {
            if (p == parent) { matches = true; break; }
        }
        if (!matches) continue;
        const auto node = fs::path("/dev/input") / entry.path().filename();
        if (node == alreadyGrabbed) continue; // The source must own this grab to receive its events.
        const auto fd = ::open(node.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { error = "Cannot open " + node.string() + ": " + std::strerror(errno); return false; }
        std::array<unsigned char, (KEY_MAX + 8) / 8> keys{};
        if (::ioctl(fd, EVIOCGBIT(EV_KEY, keys.size()), keys.data()) < 0) {
            error = "Cannot inspect " + node.string(); ::close(fd); return false;
        }
        // Never grab the receiver's keyboard or mouse collections.
        const auto has = [&](unsigned code) { return (keys[code / 8] & (1U << (code % 8))) != 0; };
        if (!has(BTN_GAMEPAD) && !has(BTN_JOYSTICK)) { ::close(fd); continue; }
        if (::ioctl(fd, EVIOCGRAB, 1) < 0) {
            error = "Cannot exclusively grab " + node.string() + ": " + std::strerror(errno);
            ::close(fd); return false;
        }
        fds_.push_back(fd);
    }
    if (fds_.empty() && alreadyGrabbed.empty()) { error = "No physical gamepad event node found; use --no-grab only if input isolation is managed separately"; return false; }
    return true;
}
} // namespace asb::platform::linux_backend
