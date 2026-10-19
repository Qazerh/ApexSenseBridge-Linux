// SPDX-License-Identifier: GPL-3.0-only
// Trigger mechanism feedback adapted from the project's patched VIIPER v0.7.0
// backend. See THIRD_PARTY_NOTICES.md and packaging/linux/VIIPER-LICENSE.txt.
#include "platform/linux/UhidDevice.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <unistd.h>

namespace asb::platform::linux_backend {
namespace {
// Mirror the existing VIIPER backend's mechanism-state feedback. Some games
// use it to observe a weapon's break point, in addition to reading the axis.
std::pair<std::uint8_t, std::uint8_t> triggerState(const std::array<std::uint8_t, 11>& effect,
                                                std::uint8_t position) {
    if (effect[0] != 0x21 && effect[0] != 0x25 && effect[0] != 0x26) return {9, 0};
    const unsigned mask = effect[1] | (unsigned(effect[2]) << 8);
    unsigned first = 10, last = 10, count = 0;
    for (unsigned zone = 0; zone < 10; ++zone) {
        if (mask & (1U << zone)) { if (count++ == 0) first = zone; last = zone; }
    }
    if (!count || (effect[0] == 0x25 && count < 2)) return {9, 0};
    const unsigned zone = unsigned(position) * 10 / 256;
    if (effect[0] == 0x25) {
        return {static_cast<std::uint8_t>(zone < first ? first : zone < last ? 0x10 | zone : 0x29), 2};
    }
    return {static_cast<std::uint8_t>(first | (zone >= first ? 0x10 : 0)),
            static_cast<std::uint8_t>(effect[0] == 0x21 ? 1 : 3)};
}
} // namespace

void UhidDevice::trace(std::string_view message) {
    if (!trace_) return;
    if (traceLines_ < 8192) trace_(message);
    else if (traceLines_ == 8192) trace_("Trace limit reached; further events suppressed.");
    ++traceLines_;
}

UhidDevice::~UhidDevice() { close(); }
void UhidDevice::close() noexcept {
    // Closing the descriptor destroys the virtual controller, even on failure.
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    started_ = false;
}

bool UhidDevice::send(const uhid_event& event, std::string& error) {
    return sendBytes(&event, sizeof(event), error);
}

bool UhidDevice::sendBytes(const void* data, std::size_t length, std::string& error) {
    ssize_t size;
    do { size = ::write(fd_, data, length); } while (size < 0 && errno == EINTR);
    if (size != static_cast<ssize_t>(length)) {
        error = "UHID write failed: " + std::string(size < 0 ? std::strerror(errno) : "short event");
        return false;
    }
    return true;
}

bool UhidDevice::create(const std::array<std::uint8_t, 6>& address, std::string& error) {
    if (fd_ < 0) fd_ = ::open("/dev/uhid", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        error = "Cannot open /dev/uhid: " + std::string(std::strerror(errno)) +
                ". Load the uhid module and install the udev rules in docs/linux.md.";
        return false;
    }
    address_ = address;
    uhid_event event{};
    event.type = UHID_CREATE2;
    auto& request = event.u.create2;
    std::strcpy(reinterpret_cast<char*>(request.name), "Sony Interactive Entertainment Wireless Controller");
    std::strcpy(reinterpret_cast<char*>(request.phys), "apexsensebridge/input0");
    std::snprintf(reinterpret_cast<char*>(request.uniq), sizeof(request.uniq),
                  "%02x:%02x:%02x:%02x:%02x:%02x", address[5], address[4], address[3], address[2], address[1], address[0]);
    request.bus = BUS_USB; request.vendor = 0x054c; request.product = 0x0ce6; request.version = 0x0100;
    const auto descriptor = dualSenseDescriptor();
    request.rd_size = static_cast<std::uint16_t>(descriptor.size());
    std::copy(descriptor.begin(), descriptor.end(), request.rd_data);
    trace("CREATE USB 054c:0ce6 interface=0 descriptor=273 input=64 output=48");
    return send(event, error);
}

bool UhidDevice::dispatch(const Handler& handler, std::string& error, unsigned budget) {
    // Bound each batch so a busy reader cannot starve physical input or Ctrl+C.
    for (unsigned n = 0; n < budget; ++n) {
        uhid_event event{}; // Kernel may return a short, zero-extended event.
        const auto size = ::read(fd_, &event, sizeof(event));
        if (size < 0 && (errno == EAGAIN || errno == EINTR)) return true;
        if (size < static_cast<ssize_t>(sizeof(event.type))) { error = "Cannot read UHID event"; return false; }
        const auto output = [&](std::span<const std::uint8_t> data) {
            const auto feedback = decodeUsbOutput(data);
            if (!feedback) return false;
            if (feedback->enableBits1 & 4) rightEffect_ = feedback->rightTriggerEffect;
            if (feedback->enableBits1 & 8) leftEffect_ = feedback->leftTriggerEffect;
            hostTimestamp_ = 0;
            for (unsigned i = 0; i < 4; ++i) hostTimestamp_ |= std::uint32_t(data[33 + i]) << (8 * i);
            ++outputs_;
            std::copy_n(data.begin(), outputReport_.size(), outputReport_.begin());
            if (trace_ && traceLines_ <= 8192) {
                std::ostringstream line;
                line << "OUTPUT size=" << data.size() << " bytes=" << std::hex << std::setfill('0');
                for (const auto byte : data) line << std::setw(2) << unsigned(byte) << ' ';
                trace(line.str());
            }
            handler(*feedback);
            return true;
        };
        switch (event.type) {
        case UHID_START: started_ = true; trace("START"); break;
        case UHID_STOP: started_ = false; trace("STOP"); break;
        case UHID_OPEN: trace("OPEN (kernel input consumer)"); break;
        case UHID_CLOSE: trace("CLOSE (last kernel input consumer)"); break;
        case UHID_GET_REPORT: {
            uhid_event reply{};
            reply.type = UHID_GET_REPORT_REPLY;
            reply.u.get_report_reply.id = event.u.get_report.id;
            std::vector<std::uint8_t> data;
            const auto& request = event.u.get_report;
            if (request.rtype == UHID_FEATURE_REPORT && request.rnum == 8)
                data.assign(hostSetupReport_.begin(), hostSetupReport_.end());
            else if (request.rtype == UHID_FEATURE_REPORT) data = featureReport(request.rnum, address_);
            else if (request.rtype == UHID_INPUT_REPORT && request.rnum == 1)
                data.assign(inputReport_.begin(), inputReport_.end());
            else if (request.rtype == UHID_OUTPUT_REPORT && request.rnum == 2)
                data.assign(outputReport_.begin(), outputReport_.end());
            reply.u.get_report_reply.err = data.empty() ? EOPNOTSUPP : 0;
            reply.u.get_report_reply.size = static_cast<std::uint16_t>(data.size());
            std::copy(data.begin(), data.end(), reply.u.get_report_reply.data);
            if (trace_) {
                std::ostringstream line;
                line << "GET_REPORT type=" << unsigned(request.rtype) << " number=0x"
                     << std::hex << unsigned(request.rnum) << std::dec << " bytes=" << data.size()
                     << " result=" << (data.empty() ? "unsupported" : "ok");
                trace(line.str());
            }
            if (!send(reply, error)) return false;
            break;
        }
        case UHID_OUTPUT:
            if (event.u.output.rtype == UHID_OUTPUT_REPORT && event.u.output.size <= UHID_DATA_MAX)
                if (!output({event.u.output.data, event.u.output.size})) trace("OUTPUT rejected (unknown ID or length)");
            break;
        case UHID_SET_REPORT: {
            const auto& request = event.u.set_report;
            bool accepted = false;
            if (request.rtype == UHID_OUTPUT_REPORT && request.rnum == 2 && request.size <= UHID_DATA_MAX) {
                accepted = output({request.data, request.size});
            } else if (request.rtype == UHID_FEATURE_REPORT && request.rnum == 8 &&
                       request.size > 0 && request.size <= hostSetupReport_.size() && request.data[0] == 8) {
                // Native DualSense clients send this declared host-setup feature
                // during initialization. Match the upstream virtual backend's
                // acknowledgement, retaining it only for virtual readback.
                // It must never be forwarded as configuration to the APEX.
                hostSetupReport_.fill(0);
                std::copy_n(request.data, request.size, hostSetupReport_.begin());
                accepted = true;
            }
            uhid_event reply{};
            reply.type = UHID_SET_REPORT_REPLY;
            reply.u.set_report_reply.id = request.id;
            reply.u.set_report_reply.err = accepted ? 0 : EOPNOTSUPP;
            if (trace_) {
                std::ostringstream line;
                line << "SET_REPORT type=" << unsigned(request.rtype) << " number=0x"
                     << std::hex << unsigned(request.rnum) << std::dec << " bytes=" << request.size
                     << " result=" << (accepted ? "ok" : "unsupported");
                if (request.rtype == UHID_FEATURE_REPORT && request.size <= UHID_DATA_MAX) {
                    line << " data=" << std::hex << std::setfill('0');
                    for (unsigned i = 0; i < request.size; ++i)
                        line << std::setw(2) << unsigned(request.data[i]) << ' ';
                }
                trace(line.str());
            }
            if (!send(reply, error)) return false;
            break;
        }
        default: break; // OPEN/CLOSE do not gate I/O in UHID.
        }
    }
    return true;
}

bool UhidDevice::update(const dualsense::DualSenseInputState& state, std::string& error) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto timestamp = static_cast<std::uint32_t>(static_cast<std::uint64_t>(elapsed) * 3); // 3 MHz
    auto report = buildUsbInput(state, sequence_++, timestamp);
    const auto right = triggerState(rightEffect_, state.r2);
    const auto left = triggerState(leftEffect_, state.l2);
    report[42] = right.first; report[43] = left.first;
    report[48] = right.second | (left.second << 4);
    for (unsigned i = 0; i < 4; ++i) {
        report[44 + i] = static_cast<std::uint8_t>(hostTimestamp_ >> (8 * i));
        report[49 + i] = static_cast<std::uint8_t>(timestamp >> (8 * i));
    }
    inputReport_ = report;
    // UHID zero-extends short writes. Include the entire input payload in one
    // write, without copying the union's unused 4 KB tail on every input sample.
    // https://github.com/torvalds/linux/blob/master/drivers/hid/uhid.c (uhid_char_write)
    constexpr auto dataOffset = offsetof(uhid_event, u.input2.data);
    std::array<std::uint8_t, dataOffset + 64> event{};
    const std::uint32_t type = UHID_INPUT2;
    const auto size = static_cast<std::uint16_t>(report.size());
    std::memcpy(event.data(), &type, sizeof(type));
    std::memcpy(event.data() + offsetof(uhid_event, u.input2.size), &size, sizeof(size));
    std::copy(report.begin(), report.end(), event.begin() + dataOffset);
    return sendBytes(event.data(), event.size(), error);
}
} // namespace asb::platform::linux_backend
