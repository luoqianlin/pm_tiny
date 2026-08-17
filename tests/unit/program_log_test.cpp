#include "program_log.h"
#include "frame_stream.hpp"
#include "rotating_log_writer.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "program_log_test failure: %s\n", message);
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
    return std::string(directory) + "pm_tiny_logging_test_" + std::to_string(GetCurrentProcessId()) +
           "\\nested\\app.log";
#else
    return "/tmp/pm_tiny_logging_test_" + std::to_string(static_cast<long long>(getpid())) +
           "/nested/app.log";
#endif
}

void remove_artifacts(const std::string &path) {
    for (int index = 0; index <= 4; ++index) {
        const std::string item = index == 0 ? path : path + "." + std::to_string(index);
        std::remove(item.c_str());
    }
}

} // namespace

int main() {
    pm_tiny::log_mode_t mode;
    require(pm_tiny::parse_log_mode("split", mode) && mode == pm_tiny::log_mode_t::split,
            "parse split");
    require(pm_tiny::parse_log_mode("combined", mode) && mode == pm_tiny::log_mode_t::combined,
            "parse combined");
    require(!pm_tiny::parse_log_mode("invalid", mode), "reject invalid mode");

    require(pm_tiny::derive_log_file_names("api", pm_tiny::log_mode_t::combined, "")[0] == "api.log",
            "combined default file");
    const auto split = pm_tiny::derive_log_file_names("api", pm_tiny::log_mode_t::split, "custom.log");
    require(split.size() == 2 && split[0] == "custom_stdout.log" && split[1] == "custom_stderr.log",
            "split file derivation");

    pm_tiny::bounded_log_tail tail(5);
    tail.append("abc", 3);
    tail.append("defg", 4);
    require(tail.snapshot() == "cdefg", "ring tail wrap");
    require(tail.begin_offset() == 2 && tail.total_bytes() == 7, "ring offsets");
    require(tail.read(4, 2) == "ef", "ring read offset");

    pm_tiny::program_log_request log_request;
    log_request.name = "中文-app";
    log_request.mode = pm_tiny::log_request_mode::history;
    std::vector<std::uint8_t> log_request_payload;
    pm_tiny::append_program_log_request(log_request_payload, log_request);
    const auto decoded_log_request = pm_tiny::read_program_log_request(log_request_payload);
    require(decoded_log_request.name == log_request.name &&
            decoded_log_request.mode == pm_tiny::log_request_mode::history,
            "program log request roundtrip");

    pm_tiny::program_log_response log_response;
    log_response.mode = pm_tiny::log_request_mode::history;
    log_response.generation = 7;
    log_response.last_pid = 1234;
    log_response.last_exit_time_unix_ms = 1723766400000LL;
    log_response.exit_reason = "exited";
    log_response.exit_code = 0;
    std::vector<std::uint8_t> log_response_payload;
    pm_tiny::append_program_log_response(log_response_payload, log_response);
    pm_tiny::iframe_stream log_response_stream(log_response_payload);
    const auto decoded_log_response = pm_tiny::read_program_log_response(log_response_stream);
    require(decoded_log_response.generation == 7 && decoded_log_response.last_pid == 1234 &&
            decoded_log_response.mode == pm_tiny::log_request_mode::history,
            "program log response roundtrip");
    require(pm_tiny::format_program_log_history_header("中文-app", decoded_log_response) ==
            "[pm_tiny] showing cached log for stopped process `中文-app`; generation=7 "
            "last_exit=2024-08-16T00:00:00Z pid=1234 reason=exited code=0\n",
            "program log history header");

    pm_tiny::log_sink_health health;
    health.record_failure(100, 7, "open failed");
    require(health.degraded && health.dropped_bytes == 7 && !health.retry_ready(1099) &&
            health.retry_ready(1100), "first retry delay");
    health.record_failure(1100, 3, "write failed");
    require(health.retry_due_ms == 3100 && health.dropped_bytes == 10, "retry backoff");
    health.record_recovery();
    require(!health.degraded && health.retry_delay_ms == 1000, "health recovery");

    const std::string path = temp_path();
    remove_artifacts(path);
    std::string error;
    {
        pm_tiny::rotating_log_writer writer(path, 4, 2);
        require(writer.open(error), error.c_str());
        require(writer.append("abcdefghij", 10, error), error.c_str());
        require(writer.flush(error), error.c_str());
    }
    require(read_file(path) == "ij", "current rotated content");
    require(read_file(path + ".1") == "efgh", "newest archive");
    require(read_file(path + ".2") == "abcd", "oldest archive");
    remove_artifacts(path);
    {
        pm_tiny::rotating_log_writer writer(path, 4, 0);
        require(writer.append("abcdef", 6, error), error.c_str());
    }
    require(read_file(path) == "ef", "zero archive truncation");
    remove_artifacts(path);
    return 0;
}
