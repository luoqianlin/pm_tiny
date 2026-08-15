#include "pm_tiny_sdk.h"
#include "pm_tiny_sdk.hpp"
#include "pm_tiny.h"
#include "protocol_v3.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

void expect(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

std::string socket_path(const std::string &tag) {
    return "/tmp/pm_tiny_sdk_" + std::to_string(static_cast<long long>(::getpid())) + "_" + tag + ".sock";
}

int listen_socket(const std::string &path) {
    ::unlink(path.c_str());
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket failed");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
        ::listen(fd, 8) < 0) {
        ::close(fd);
        throw std::runtime_error("listen failed");
    }
    return fd;
}

std::vector<pm_tiny::protocol_message> read_messages(int fd, std::size_t expected) {
    pm_tiny::protocol_decoder decoder;
    std::vector<pm_tiny::protocol_message> messages;
    std::uint8_t buffer[512];
    while (messages.size() < expected) {
        while (!decoder.empty()) messages.push_back(decoder.pop());
        if (messages.size() >= expected) break;
        const ssize_t count = ::read(fd, buffer, sizeof(buffer));
        if (count <= 0) throw std::runtime_error("read failed");
        decoder.feed(buffer, static_cast<std::size_t>(count));
    }
    return messages;
}

pm_tiny::protocol_message read_message(int fd) {
    return read_messages(fd, 1).front();
}

std::size_t fd_count() {
    DIR *directory = ::opendir("/proc/self/fd");
    if (directory == nullptr) throw std::runtime_error("opendir failed");
    std::size_t count = 0;
    while (::readdir(directory) != nullptr) ++count;
    ::closedir(directory);
    return count;
}

std::size_t rss_kib() {
    std::ifstream stream("/proc/self/statm");
    std::size_t total_pages = 0;
    std::size_t resident_pages = 0;
    stream >> total_pages >> resident_pages;
    if (!stream) throw std::runtime_error("cannot read process RSS");
    (void)total_pages;
    return resident_pages * static_cast<std::size_t>(::sysconf(_SC_PAGESIZE)) / 1024;
}

void test_disabled_and_c_abi() {
    pm_tiny::client_config config;
    config.app_name.clear();
    config.endpoint.clear();
    ::unsetenv("PM_TINY_APP_NAME");
    ::unsetenv("PM_TINY_SOCK_FILE");
    pm_tiny::client client(config);
    expect(client.tick() == pm_tiny::enqueue_result::disabled, "disabled result mismatch");
    expect(client.flush(std::chrono::milliseconds(1)), "disabled flush failed");

    pm_tiny_client_config_t c_config{};
    c_config.struct_size = sizeof(c_config);
    c_config.abi_version = PM_TINY_SDK_ABI_VERSION;
    c_config.uds_abstract_namespace = 0;
    pm_tiny_client_t *handle = nullptr;
    expect(pm_tiny_client_create(&c_config, &handle) == 0 && handle != nullptr, "C create failed");
    expect(pm_tiny_client_tick(handle) == PM_TINY_ENQUEUE_DISABLED, "C disabled mismatch");
    pm_tiny_client_status_t status{};
    status.struct_size = sizeof(status);
    status.abi_version = PM_TINY_SDK_ABI_VERSION;
    expect(pm_tiny_client_status(handle, &status) == 0 && status.enabled == 0, "C status failed");
    pm_tiny_client_destroy(handle);
    c_config.abi_version = 3;
    expect(pm_tiny_client_create(&c_config, &handle) == -1, "old ABI was accepted");
}

void test_persistent_connection() {
    const std::string path = socket_path("persistent");
    const int listener = listen_socket(path);
    std::vector<std::uint16_t> types;
    std::thread server([&]() {
        const int connection = ::accept(listener, nullptr, nullptr);
        const auto messages = read_messages(connection, 2);
        types.push_back(messages[0].type);
        types.push_back(messages[1].type);
        ::close(connection);
    });
    pm_tiny::client_config config;
    config.app_name = "persistent";
    config.endpoint = path;
    config.uds_abstract_namespace = 0;
    pm_tiny::client client(config);
    expect(client.ready() == pm_tiny::enqueue_result::queued, "ready was not queued");
    expect(client.flush(std::chrono::seconds(2)), "ready flush timed out");
    expect(client.tick() == pm_tiny::enqueue_result::queued, "tick was not queued");
    expect(client.flush(std::chrono::seconds(2)), "tick flush timed out");
    client.close();
    server.join();
    ::close(listener);
    ::unlink(path.c_str());
    expect(types.size() == 2 && types[0] == PM_TINY_FRAME_TYPE_APP_READY &&
           types[1] == PM_TINY_FRAME_TYPE_APP_TICK, "persistent connection frames mismatch");
}

void test_coalescing_and_delayed_reconnect() {
    const std::string path = socket_path("coalesce");
    ::unlink(path.c_str());
    pm_tiny::client_config config;
    config.app_name = "coalesce";
    config.endpoint = path;
    config.uds_abstract_namespace = 0;
    pm_tiny::client client(config);
    for (int i = 0; i < 100000; ++i) client.tick();
    expect(client.ready() == pm_tiny::enqueue_result::queued, "ready slot was not queued");
    const pm_tiny::client_status pending = client.status();
    expect(pending.pending_ready && pending.pending_tick, "fixed event slots were not pending");
    expect(pending.tick_coalesced > 99000, "tick calls were not coalesced");

    const int listener = listen_socket(path);
    std::vector<std::uint16_t> received;
    std::thread server([&]() {
        const int connection = ::accept(listener, nullptr, nullptr);
        const auto messages = read_messages(connection, 2);
        received.push_back(messages[0].type);
        received.push_back(messages[1].type);
        ::close(connection);
    });
    expect(client.flush(std::chrono::seconds(3)), "delayed reconnect flush timed out");
    client.close();
    server.join();
    ::close(listener);
    ::unlink(path.c_str());
    expect(received.size() == 2 && received[0] == PM_TINY_FRAME_TYPE_APP_READY &&
           received[1] == PM_TINY_FRAME_TYPE_APP_TICK, "ready priority or coalesced tick mismatch");
}

void test_one_hundred_reconnects() {
    const std::string path = socket_path("reconnect");
    const int listener = listen_socket(path);
    std::atomic<int> received{0};
    std::thread server([&]() {
        while (received.load() < 100) {
            const int connection = ::accept(listener, nullptr, nullptr);
            linger reset{1, 0};
            ::setsockopt(connection, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
            read_message(connection);
            ++received;
            ::close(connection);
        }
    });
    pm_tiny::client_config config;
    config.app_name = "reconnect";
    config.endpoint = path;
    config.uds_abstract_namespace = 0;
    pm_tiny::client client(config);
    for (int i = 0; i < 100; ++i) {
        client.tick();
        expect(client.flush(std::chrono::seconds(3)), "reconnect flush timed out");
        while (received.load() <= i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    client.close();
    server.join();
    ::close(listener);
    ::unlink(path.c_str());
    expect(received.load() == 100, "reconnect count mismatch");
}

void test_close_latency_and_fd_cleanup() {
    const std::size_t before = fd_count();
    for (int i = 0; i < 10; ++i) {
        pm_tiny::client_config warmup;
        warmup.app_name = "warmup";
        warmup.endpoint = socket_path("warmup_" + std::to_string(i));
        warmup.uds_abstract_namespace = 0;
        pm_tiny::client client(warmup);
        client.ready();
        client.close();
    }
    const std::size_t rss_before = rss_kib();
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        pm_tiny::client_config config;
        config.app_name = "close";
        config.endpoint = socket_path("missing_" + std::to_string(i));
        config.uds_abstract_namespace = 0;
        pm_tiny::client client(config);
        client.ready();
        client.close();
        expect(client.ready() == pm_tiny::enqueue_result::stopped, "closed client accepted event");
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    expect(elapsed < std::chrono::seconds(1), "close exceeded one second");
    expect(fd_count() == before, "file descriptor count grew");
#if !defined(__SANITIZE_ADDRESS__)
    expect(rss_kib() <= rss_before + 1024, "resident memory grew across client close cycles");
#else
    (void)rss_before;
#endif
}

} // namespace

int main() {
    try {
        test_disabled_and_c_abi();
        test_persistent_connection();
        test_coalescing_and_delayed_reconnect();
        test_one_hundred_reconnects();
        test_close_latency_and_fd_cleanup();
        std::cout << "sdk_client_test passed" << std::endl;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
