#ifndef PM_TINY_PERSISTENCE_WORKER_H
#define PM_TINY_PERSISTENCE_WORKER_H

#include <functional>
#include <mutex>
#include <thread>

namespace pm_tiny {

class persistence_worker {
public:
    persistence_worker() = default;
    ~persistence_worker();

    persistence_worker(const persistence_worker &) = delete;
    persistence_worker &operator=(const persistence_worker &) = delete;

    bool submit(std::function<int()> task);
    bool poll(int &result);
    bool busy() const;
    void wait();

private:
    mutable std::mutex mutex_;
    std::thread thread_;
    bool running_ = false;
    bool completed_ = false;
    int result_ = -1;
};

} // namespace pm_tiny

#endif
