#include "daemon_log.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "daemon_log_test failure: %s\n", message);
        std::exit(1);
    }
}

std::string read_file(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string temp_path() {
#ifdef _WIN32
    char directory[MAX_PATH] = {};
    require(GetTempPathA(MAX_PATH, directory) != 0, "GetTempPathA");
    return std::string(directory) + "pm_tiny_daemon_log_test_" +
           std::to_string(GetCurrentProcessId()) + ".log";
#else
    return "/tmp/pm_tiny_daemon_log_test_" +
           std::to_string(static_cast<long long>(getpid())) + ".log";
#endif
}

std::size_t count_occurrences(const std::string &text, const std::string &needle) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

} // namespace

int main() {
    pm_tiny::reset_daemon_log();
    auto snapshot = pm_tiny::daemon_log_snapshot();
    require(snapshot.sink == "console" && !snapshot.degraded, "console sink snapshot");
    pm_tiny::daemon_log_message(pm_tiny::daemon_log_level_t::info,
                                "safe before configuration");

    pm_tiny::daemon_log_level_t level;
    require(pm_tiny::parse_daemon_log_level("debug", level) &&
            level == pm_tiny::daemon_log_level_t::debug, "parse debug");
    require(pm_tiny::parse_daemon_log_level("fatal", level) &&
            level == pm_tiny::daemon_log_level_t::fatal, "parse fatal");
    require(!pm_tiny::parse_daemon_log_level("trace", level), "reject invalid level");

    const std::string path = temp_path();
    std::remove(path.c_str());
    pm_tiny::daemon_log_config_t config;
    config.path = path;
    config.max_size_bytes = 1024U * 1024U;
    config.archive_count = 1;
    config.mirror_console = false;
    config.minimum_level = pm_tiny::daemon_log_level_t::warn;
    std::string error;
    require(pm_tiny::configure_daemon_log(config, error), error.c_str());
    snapshot = pm_tiny::daemon_log_snapshot();
    require(snapshot.sink == "file" && snapshot.level == "warn" && !snapshot.degraded,
            "file sink snapshot");
    pm_tiny::daemon_log_message(pm_tiny::daemon_log_level_t::info, "filtered info");
    pm_tiny::daemon_log_message(pm_tiny::daemon_log_level_t::warn, "visible warning");
    errno = ENOENT;
    PM_TINY_DLOG_ERROR_ERRNO("open test resource");
    pm_tiny::reset_daemon_log();

    const std::string filtered = read_file(path);
    require(filtered.find("filtered info") == std::string::npos, "level filter");
    require(filtered.find("[warn] [daemon] visible warning") != std::string::npos,
            "daemon text format");
    require(filtered.find("[error] [daemon] [daemon_log_test.cpp:") != std::string::npos,
            "source context format");
    require(filtered.find("open test resource") != std::string::npos, "errno message");

    config.minimum_level = pm_tiny::daemon_log_level_t::debug;
    require(pm_tiny::configure_daemon_log(config, error), error.c_str());
    std::vector<std::thread> threads;
    for (int thread_index = 0; thread_index < 4; ++thread_index) {
        threads.emplace_back([thread_index]() {
            for (int line = 0; line < 50; ++line)
                PM_TINY_DLOG_INFO("thread=%d line=%d marker", thread_index, line);
        });
    }
    for (auto &thread : threads) thread.join();
    pm_tiny::reset_daemon_log();
    const std::string concurrent = read_file(path);
    require(count_occurrences(concurrent, " marker\n") == 200, "thread-safe complete lines");

    {
        std::ofstream blocker(path, std::ios::binary | std::ios::trunc);
        blocker << "block parent directory";
    }
    config.path = path + "/child.log";
    require(!pm_tiny::configure_daemon_log(config, error) && !error.empty(),
            "file open failure falls back without throwing");
    snapshot = pm_tiny::daemon_log_snapshot();
    require(snapshot.sink == "console_fallback" && snapshot.degraded && !snapshot.last_error.empty(),
            "fallback sink snapshot");
    pm_tiny::daemon_log_message(pm_tiny::daemon_log_level_t::error,
                                "safe after file open failure");
    pm_tiny::reset_daemon_log();
#ifndef _WIN32
    config.path = "/dev/full";
    require(pm_tiny::configure_daemon_log(config, error), "open /dev/full");
    pm_tiny::daemon_log_message(pm_tiny::daemon_log_level_t::error, "force write failure");
    snapshot = pm_tiny::daemon_log_snapshot();
    require(snapshot.sink == "console_fallback" && snapshot.degraded,
            "write failure fallback snapshot");
    pm_tiny::reset_daemon_log();
#endif
    std::remove(path.c_str());
    return 0;
}
