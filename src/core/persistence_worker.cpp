#include "persistence_worker.h"

#include <utility>

namespace pm_tiny {

persistence_worker::~persistence_worker() {
    wait();
}

bool persistence_worker::submit(std::function<int()> task) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_ || completed_ || thread_.joinable()) return false;
    running_ = true;
    result_ = -1;
    thread_ = std::thread([this, task = std::move(task)]() mutable {
        int result = -1;
        try {
            result = task();
        } catch (...) {
            result = -1;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = result;
        running_ = false;
        completed_ = true;
    });
    return true;
}

bool persistence_worker::poll(int &result) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!completed_) return false;
        result = result_;
        completed_ = false;
    }
    if (thread_.joinable()) thread_.join();
    return true;
}

bool persistence_worker::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ || completed_;
}

void persistence_worker::wait() {
    if (thread_.joinable()) thread_.join();
}

} // namespace pm_tiny
