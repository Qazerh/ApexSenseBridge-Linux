#include "platform/linux/AsyncDiagnostics.h"

#include <exception>
#include <stdexcept>

namespace asb::platform::linux_backend {

AsyncDiagnostics::AsyncDiagnostics() : worker_([this] { run(); }) {}
AsyncDiagnostics::~AsyncDiagnostics() { finish(); }

bool AsyncDiagnostics::submit(std::function<void()> task) {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock || stopping_ || size_ == kCapacity || failed()) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    queue_[tail_] = std::move(task);
    tail_ = (tail_ + 1) % kCapacity;
    ++size_;
    lock.unlock();
    cv_.notify_one();
    return true;
}

void AsyncDiagnostics::submitRequired(std::function<void()> task) {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return size_ < kCapacity || stopping_ || failed(); });
    if (failed()) throw std::runtime_error(error_);
    if (stopping_) throw std::runtime_error("Diagnostics worker stopped");
    queue_[tail_] = std::move(task);
    tail_ = (tail_ + 1) % kCapacity;
    ++size_;
    lock.unlock();
    cv_.notify_one();
}

void AsyncDiagnostics::run() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || size_; });
            if (!size_) return;
            task = std::move(queue_[head_]);
            head_ = (head_ + 1) % kCapacity;
            --size_;
        }
        cv_.notify_all();
        try { task(); }
        catch (const std::exception& error) {
            std::lock_guard lock(mutex_);
            error_ = error.what();
            failed_.store(true, std::memory_order_release);
        } catch (...) {
            std::lock_guard lock(mutex_);
            error_ = "Background diagnostics failed";
            failed_.store(true, std::memory_order_release);
        }
    }
}

std::string AsyncDiagnostics::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

void AsyncDiagnostics::finish() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

} // namespace asb::platform::linux_backend
