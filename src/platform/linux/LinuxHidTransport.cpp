#include "platform/HidTransport.h"
#include "platform/linux/HidReportDescriptor.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iomanip>
#include <sstream>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace asb::platform {
namespace {
namespace fs = std::filesystem;

std::wstring wide(const std::string& s) { return {s.begin(), s.end()}; }
std::string readText(const fs::path& path) {
    std::ifstream input(path);
    std::string text;
    std::getline(input, text);
    return text;
}

class LinuxHidTransport final : public HidTransport {
public:
    LinuxHidTransport(int fd, HidDeviceInfo info) : fd_(fd), info_(std::move(info)) {}
    ~LinuxHidTransport() override { ::close(fd_); }
    bool isOpen() const noexcept override { return fd_ >= 0; }
    const HidDeviceInfo& info() const noexcept override { return info_; }
    bool writeOutputReport(std::span<const std::uint8_t> report, std::string& error) override {
        if (report.empty() || report.size() > info_.outputReportLength) {
            error = "Output report exceeds the HID descriptor's declared size";
            return false;
        }
        // V1 commands are shorter than their declared 32/64-byte output report.
        // Match upstream's Windows backend padding, including the report ID.
        std::vector<std::uint8_t> wire(info_.outputReportLength, 0);
        std::copy(report.begin(), report.end(), wire.begin());
        ssize_t written;
        do { written = ::write(fd_, wire.data(), wire.size()); } while (written < 0 && errno == EINTR);
        if (written != static_cast<ssize_t>(wire.size())) {
            error = written < 0 ? std::strerror(errno) : "Short HID output write";
            return false;
        }
        return true;
    }
    bool readFeatureReport(std::span<std::uint8_t> report, std::string& error) override {
        if (report.empty() || report.size() > 4096) { error = "Invalid feature report size"; return false; }
        if (::ioctl(fd_, HIDIOCGFEATURE(report.size()), report.data()) < 0) {
            error = std::strerror(errno); return false;
        }
        return true;
    }
    HidReadStatus readInputReport(std::span<std::uint8_t> report, std::chrono::milliseconds timeout,
                                 std::size_t& bytesRead, std::string& error) override {
        bytesRead = 0;
        pollfd event{fd_, POLLIN, 0};
        const int ready = ::poll(&event, 1, static_cast<int>(std::clamp<std::int64_t>(timeout.count(), 0, 60000)));
        if (ready == 0 || (ready < 0 && errno == EINTR)) return HidReadStatus::Timeout;
        if (ready < 0 || (event.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            error = ready < 0 ? std::strerror(errno) : "Controller disconnected";
            return HidReadStatus::Error;
        }
        const auto count = ::read(fd_, report.data(), report.size());
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) return HidReadStatus::Timeout;
        if (count <= 0) {
            error = count < 0 ? std::strerror(errno) : "Controller disconnected";
            return HidReadStatus::Error;
        }
        bytesRead = static_cast<std::size_t>(count);
        return HidReadStatus::Data;
    }
private:
    int fd_;
    HidDeviceInfo info_;
};
} // namespace

std::vector<HidDeviceInfo> enumerateHidDevices(std::string& error, HidStringQuery) {
    error.clear();
    std::vector<HidDeviceInfo> result;
    std::error_code ec;
    fs::directory_iterator entries("/sys/class/hidraw", ec);
    if (ec == std::errc::no_such_file_or_directory) return result; // No hidraw devices/subsystem present.
    if (ec) { error = "Cannot enumerate /sys/class/hidraw: " + ec.message(); return result; }
    for (const auto& entry : entries) {
        const auto device = entry.path() / "device";
        std::ifstream descriptor(device / "report_descriptor", std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(descriptor), {}};
        const auto layout = linux_backend::parseHidReportDescriptor(bytes);
        if (!layout) continue;
        HidDeviceInfo info;
        info.path = (fs::path("/dev") / entry.path().filename()).wstring();
        info.usagePage = layout->usagePage;
        info.usage = layout->usage;
        info.inputReportLength = layout->inputLength;
        info.outputReportLength = layout->outputBufferLength();
        info.featureReportLength = layout->featureBufferLength();
        std::ifstream uevent(device / "uevent");
        for (std::string line; std::getline(uevent, line);) {
            if (line.starts_with("HID_ID=")) {
                auto id = line.substr(7);
                std::replace(id.begin(), id.end(), ':', ' ');
                unsigned bus = 0, vendor = 0, product = 0;
                std::istringstream(id) >> std::hex >> bus >> vendor >> product;
                info.vendorId = static_cast<std::uint16_t>(vendor);
                info.productId = static_cast<std::uint16_t>(product);
            } else if (line.starts_with("HID_NAME=")) info.product = wide(line.substr(9));
            else if (line.starts_with("HID_UNIQ=")) info.serial = wide(line.substr(9));
        }
        auto parent = fs::canonical(device, ec);
        info.instanceId = parent.wstring();
        // USB topology, shared by vendor HID and xpad/evdev interfaces.
        for (; !ec && parent.has_relative_path(); parent = parent.parent_path()) {
            if (fs::exists(parent / "bInterfaceNumber", ec)) {
                unsigned number = 0;
                std::istringstream(readText(parent / "bInterfaceNumber")) >> std::hex >> number;
                std::wostringstream name;
                name << L"MI_" << std::uppercase << std::hex << std::setfill(L'0') << std::setw(2) << number;
                info.interfaceNumber = name.str();
            }
            if (fs::exists(parent / "idVendor", ec)) {
                info.containerId = parent.wstring();
                info.manufacturer = wide(readText(parent / "manufacturer"));
                break;
            }
        }
        result.push_back(std::move(info));
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    return result;
}

HidTransport* createHidTransport(const HidDeviceInfo& info, std::string& error) {
    const auto path = fs::path(info.path).string();
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        error = "Cannot open " + path + ": " + std::strerror(errno) + ". See docs/linux.md for udev permissions.";
        return nullptr;
    }
    // Also prevent two bridge/test processes from fighting over the motors.
    if (::flock(fd, LOCK_EX | LOCK_NB) < 0) {
        error = "Another bridge holds " + path;
        ::close(fd); return nullptr;
    }
    hidraw_devinfo identity{};
    hidraw_report_descriptor descriptor{};
    int size = 0;
    if (::ioctl(fd, HIDIOCGRAWINFO, &identity) < 0 ||
        static_cast<std::uint16_t>(identity.vendor) != info.vendorId ||
        static_cast<std::uint16_t>(identity.product) != info.productId ||
        ::ioctl(fd, HIDIOCGRDESCSIZE, &size) < 0 || size <= 0 || size > HID_MAX_DESCRIPTOR_SIZE) {
        error = "HID identity changed or descriptor is unavailable; enumerate again";
        ::close(fd); return nullptr;
    }
    descriptor.size = static_cast<unsigned>(size);
    if (::ioctl(fd, HIDIOCGRDESC, &descriptor) < 0) {
        error = "Cannot read HID descriptor"; ::close(fd); return nullptr;
    }
    const auto layout = linux_backend::parseHidReportDescriptor({descriptor.value, descriptor.size});
    if (!layout || layout->usagePage != info.usagePage || layout->usage != info.usage) {
        error = "HID interface changed; enumerate again"; ::close(fd); return nullptr;
    }
    auto current = info;
    current.inputReportLength = layout->inputLength;
    current.outputReportLength = layout->outputBufferLength();
    current.featureReportLength = layout->featureBufferLength();
    return new LinuxHidTransport(fd, std::move(current));
}

void destroyHidTransport(HidTransport* transport) noexcept { delete transport; }
} // namespace asb::platform
