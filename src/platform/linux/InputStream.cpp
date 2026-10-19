#include "platform/linux/InputStream.h"
#include "platform/linux/HidReportDescriptor.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace asb::platform::linux_backend {

InputStream::~InputStream() { if (fd_ >= 0) ::close(fd_); }

bool InputStream::open(const HidDeviceInfo& info, std::string& error) {
    if (fd_ >= 0) { error = "Input stream is already open"; return false; }
    const auto path = std::filesystem::path(info.path).string();
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { error = "Cannot open independent Apex input stream: " + std::string(std::strerror(errno)); return false; }
    hidraw_devinfo identity{};
    hidraw_report_descriptor descriptor{};
    int size = 0;
    if (::ioctl(fd, HIDIOCGRAWINFO, &identity) < 0 ||
        static_cast<std::uint16_t>(identity.vendor) != info.vendorId ||
        static_cast<std::uint16_t>(identity.product) != info.productId ||
        ::ioctl(fd, HIDIOCGRDESCSIZE, &size) < 0 || size <= 0 || size > HID_MAX_DESCRIPTOR_SIZE) {
        error = "Apex input interface changed; enumerate again"; ::close(fd); return false;
    }
    descriptor.size = static_cast<unsigned>(size);
    if (::ioctl(fd, HIDIOCGRDESC, &descriptor) < 0) {
        error = "Cannot verify Apex input descriptor"; ::close(fd); return false;
    }
    const auto layout = parseHidReportDescriptor({descriptor.value, descriptor.size});
    if (!layout || layout->usagePage != info.usagePage || layout->usage != info.usage ||
        layout->inputLength != info.inputReportLength) {
        error = "Apex input descriptor changed; enumerate again"; ::close(fd); return false;
    }
    // The command transport retains its exclusive flock; this handle only reads.
    fd_ = fd;
    return true;
}

HidReadStatus InputStream::read(std::span<std::uint8_t> report, std::chrono::milliseconds timeout,
                                 std::size_t& bytesRead, std::string& error) {
    bytesRead = 0;
    if (fd_ < 0 || report.empty()) {
        error = "Apex input stream is not open or its read buffer is empty";
        return HidReadStatus::Error;
    }
    // Runtime already waits for both descriptors together. Drain ready reports
    // with read() directly instead of polling again before every sample.
    if (timeout.count() > 0) {
        pollfd ready{fd_, POLLIN, 0};
        const int result = ::poll(&ready, 1, static_cast<int>(std::clamp<std::int64_t>(timeout.count(), 0, 60000)));
        if (result == 0 || (result < 0 && errno == EINTR)) return HidReadStatus::Timeout;
        if (result < 0 || (ready.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            error = result < 0 ? std::strerror(errno) : "Apex input stream disconnected";
            return HidReadStatus::Error;
        }
    }
    const auto size = ::read(fd_, report.data(), report.size());
    if (size < 0 && (errno == EAGAIN || errno == EINTR)) return HidReadStatus::Timeout;
    if (size <= 0) {
        error = size < 0 ? std::strerror(errno) : "Apex input stream disconnected";
        return HidReadStatus::Error;
    }
    bytesRead = static_cast<std::size_t>(size);
    return HidReadStatus::Data;
}

} // namespace asb::platform::linux_backend
