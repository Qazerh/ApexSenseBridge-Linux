#include "platform/linux/EvdevInput.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace asb::platform::linux_backend {
namespace {
template<std::size_t N> bool has(const std::array<unsigned char,N>& bits, unsigned bit) {
    return bit / 8 < N && (bits[bit / 8] & (1U << (bit % 8)));
}
bool inspect(int fd, EvdevLayout& layout) {
    std::array<unsigned char, (KEY_CNT + 7) / 8> keys{};
    std::array<unsigned char, (ABS_CNT + 7) / 8> axes{};
    if (::ioctl(fd, EVIOCGBIT(EV_KEY, keys.size()), keys.data()) < 0 ||
        ::ioctl(fd, EVIOCGBIT(EV_ABS, axes.size()), axes.data()) < 0 ||
        (!has(keys, BTN_GAMEPAD) && !has(keys, BTN_JOYSTICK))) return false;
    layout.buttonBase = has(keys, BTN_GAMEPAD) ? BTN_GAMEPAD : BTN_JOYSTICK;
    layout.rawHidButtons = has(keys, layout.buttonBase + 2) && has(keys, layout.buttonBase + 5);
    for (unsigned i = 0; i < ABS_CNT; ++i) {
        if (has(axes, i) && ::ioctl(fd, EVIOCGABS(i), &layout.axes[i]) == 0) layout.present[i] = true;
    }
    // Reject incomplete/combined-axis collections. Independent LT/RT are
    // required; no inference from a centered DirectInput trigger axis.
    for (const auto code : {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ}) {
        if (!layout.present[code] || layout.axes[code].maximum <= layout.axes[code].minimum) return false;
    }
    return true;
}
}

EvdevInput::~EvdevInput() { if (fd_ >= 0) ::close(fd_); }

bool EvdevInput::open(const HidDeviceInfo& device, bool grab, std::string& error) {
    namespace fs = std::filesystem;
    if (fd_ >= 0 || device.containerId.empty()) { error = "Cannot identify an unused physical input source"; return false; }
    const fs::path parent(device.containerId);
    std::error_code ec;
    fs::directory_iterator entries("/sys/class/input", ec);
    if (ec) { error = ec.message(); return false; }
    struct Candidate { fs::path node; bool hid = false; };
    std::vector<Candidate> candidates;
    for (const auto& entry : entries) {
        if (!entry.path().filename().string().starts_with("event")) continue;
        auto physical = fs::canonical(entry.path() / "device", ec);
        if (ec) { ec.clear(); continue; }
        bool matches = false, hid = false;
        for (auto p = physical; p.has_relative_path(); p = p.parent_path()) {
            if (p.filename().string().starts_with("0003:")) hid = true;
            if (p == parent) { matches = true; break; }
        }
        if (!matches) continue;
        const auto node = fs::path("/dev/input") / entry.path().filename();
        const int fd = ::open(node.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        EvdevLayout layout;
        if (inspect(fd, layout)) candidates.push_back({node, hid});
        ::close(fd);
    }
    // Prefer xpad's independent Xbox axes over the generic HID projection.
    if (std::any_of(candidates.begin(), candidates.end(), [](const auto& c) { return !c.hid; }))
        std::erase_if(candidates, [](const auto& c) { return c.hid; });
    if (candidates.size() != 1) {
        error = candidates.empty() ? "No Apex 6 gamepad with independent LT/RT found. Use PC/XInput mode and check event-node permissions."
                                   : "More than one gamepad collection belongs to this controller; cannot choose safely.";
        return false;
    }
    path_ = candidates.front().node;
    bool writable = true;
    fd_ = ::open(path_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) { writable = false; fd_ = ::open(path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC); }
    input_id id{};
    if (fd_ < 0 || ::ioctl(fd_, EVIOCGID, &id) < 0 || id.vendor != device.vendorId || id.product != device.productId || !inspect(fd_, layout_)) {
        error = "Physical gamepad changed or could not be opened"; return false;
    }
    layout_.hidTriggerOrder = candidates.front().hid;
    if (!resync(error)) return false;
    if (grab && ::ioctl(fd_, EVIOCGRAB, 1) < 0) { error = "Cannot exclusively grab " + path_ + ": " + std::strerror(errno); return false; }
    std::array<unsigned char, (FF_CNT + 7) / 8> ff{};
    rumble_ = writable && ::ioctl(fd_, EVIOCGBIT(EV_FF, ff.size()), ff.data()) >= 0 && has(ff, FF_RUMBLE);
    return true;
}

bool EvdevInput::resync(std::string& error) {
    std::array<unsigned char, (KEY_CNT + 7) / 8> keys{};
    if (::ioctl(fd_, EVIOCGKEY(keys.size()), keys.data()) < 0) { error = "Cannot refresh gamepad buttons"; return false; }
    for (unsigned i = 0; i < KEY_CNT; ++i) layout_.keys[i] = has(keys, i);
    for (unsigned i = 0; i < ABS_CNT; ++i) {
        if (layout_.present[i] && ::ioctl(fd_, EVIOCGABS(i), &layout_.axes[i]) < 0) {
            error = "Cannot refresh gamepad axes"; return false;
        }
    }
    return true;
}

dualsense::DualSenseInputState EvdevInput::snapshot() const {
    dualsense::DualSenseInputState s;
    const auto axis = [&](unsigned code) {
        const auto& a = layout_.axes[code];
        const auto range = std::int64_t(a.maximum) - a.minimum;
        return static_cast<std::uint8_t>(range > 0 ? std::clamp<std::int64_t>(((std::int64_t(a.value) - a.minimum) * 255 + range / 2) / range, 0, 255) : 0);
    };
    s.lx = axis(ABS_X); s.ly = axis(ABS_Y); s.rx = axis(ABS_RX); s.ry = axis(ABS_RY);
    s.l2 = axis(layout_.hidTriggerOrder ? ABS_RZ : ABS_Z);
    s.r2 = axis(layout_.hidTriggerOrder ? ABS_Z : ABS_RZ);
    using namespace dualsense::button;
    // xpad preserves historical BTN_X/BTN_Y codes (their N/W aliases are
    // misleading). hid-generic maps usages 1..11 to contiguous key codes.
    const std::pair<unsigned, std::uint16_t> buttons[]{{BTN_A,kCross},{BTN_B,kCircle},{BTN_X,kSquare},{BTN_Y,kTriangle},
        {BTN_TL,kL1},{BTN_TR,kR1},{BTN_SELECT,kTouchpadClick},{BTN_START,kOptions},{BTN_THUMBL,kL3},{BTN_THUMBR,kR3},{BTN_MODE,kPs}};
    for (unsigned i = 0; i < std::size(buttons); ++i) {
        const auto [code, flag] = buttons[i];
        if (layout_.keys[layout_.rawHidButtons ? layout_.buttonBase + i : code]) s.buttons |= flag;
    }
    if (s.l2 > 30) s.buttons |= kL2;
    if (s.r2 > 30) s.buttons |= kR2;
    if (layout_.keys[BTN_DPAD_UP] || (layout_.present[ABS_HAT0Y] && layout_.axes[ABS_HAT0Y].value < 0)) s.dpad |= 1;
    if (layout_.keys[BTN_DPAD_DOWN] || (layout_.present[ABS_HAT0Y] && layout_.axes[ABS_HAT0Y].value > 0)) s.dpad |= 2;
    if (layout_.keys[BTN_DPAD_LEFT] || (layout_.present[ABS_HAT0X] && layout_.axes[ABS_HAT0X].value < 0)) s.dpad |= 4;
    if (layout_.keys[BTN_DPAD_RIGHT] || (layout_.present[ABS_HAT0X] && layout_.axes[ABS_HAT0X].value > 0)) s.dpad |= 8;
    return s;
}

HidReadStatus EvdevInput::read(std::optional<dualsense::DualSenseInputState>& state, std::string& error) {
    state.reset();
    // A bounded event batch preserves packet boundaries without a syscall for
    // every individual axis/button. Remaining frames stay in this local buffer.
    for (unsigned budget = 0; budget < 128; ++budget) {
        if (!buffered()) {
            const auto bytes = ::read(fd_, events_.data(), sizeof(events_));
            if (bytes < 0 && (errno == EAGAIN || errno == EINTR)) return HidReadStatus::Timeout;
            if (bytes <= 0 || bytes % sizeof(input_event)) { error = "Physical gamepad disconnected or returned malformed events"; return HidReadStatus::Error; }
            offset_ = 0; count_ = static_cast<std::size_t>(bytes) / sizeof(input_event);
        }
        const auto& e = events_[offset_++];
        if (e.type == EV_SYN && e.code == SYN_DROPPED) { lost_ = true; continue; }
        if (e.type == EV_SYN && e.code == SYN_REPORT) {
            if (lost_ && !resync(error)) return HidReadStatus::Error;
            lost_ = false; state = snapshot(); return HidReadStatus::Data;
        }
        if (lost_) continue;
        if (e.type == EV_KEY && e.code < KEY_CNT) layout_.keys[e.code] = e.value != 0;
        if (e.type == EV_ABS && e.code < ABS_CNT && layout_.present[e.code]) layout_.axes[e.code].value = e.value;
    }
    return HidReadStatus::Data; // Yield after bounded metadata/no-SYN traffic.
}

} // namespace asb::platform::linux_backend
