#include "core/persistence_worker.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>

int main() {
    pm_tiny::persistence_worker worker;
    std::atomic<bool> release{false};
    assert(worker.submit([&release]() {
        while (!release.load()) std::this_thread::yield();
        return 17;
    }));
    assert(worker.busy());
    assert(!worker.submit([]() { return 0; }));
    int result = 0;
    assert(!worker.poll(result));
    release.store(true);
    for (int attempt = 0; attempt < 100 && !worker.poll(result); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(result == 17);
    assert(!worker.busy());
    assert(worker.submit([]() { return 23; }));
    worker.wait();
    assert(worker.poll(result));
    assert(result == 23);
    assert(worker.submit([]() -> int { throw 1; }));
    worker.wait();
    assert(worker.poll(result));
    assert(result == -1);
    return 0;
}
