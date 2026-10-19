#pragma once

#include "platform/HidTransport.h"

namespace asb::platform::linux_backend {

// A separate open(), not dup(): hidraw gives each open its own report queue.
// RGB transactions can drain their command queue without consuming game input.
class InputStream {
public:
    InputStream() = default;
    explicit InputStream(int fd) : fd_(fd) {} // Takes ownership; also permits socket-based tests.
    ~InputStream();
    InputStream(const InputStream&) = delete;
    InputStream& operator=(const InputStream&) = delete;
    bool open(const HidDeviceInfo& info, std::string& error);
    int nativeHandle() const noexcept { return fd_; }
    HidReadStatus read(std::span<std::uint8_t> report, std::chrono::milliseconds timeout,
                       std::size_t& bytesRead, std::string& error);
private:
    int fd_ = -1;
};

} // namespace asb::platform::linux_backend
