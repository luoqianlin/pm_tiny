#include "daemon_info.h"
#include "daemon_info_renderer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void expect(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

template <typename Function>
void expect_decode_error(Function function, const char *message) {
    try { function(); }
    catch (const std::exception &) { return; }
    throw std::runtime_error(message);
}

pm_tiny::daemon_info_snapshot fixture() {
    pm_tiny::daemon_info_snapshot s;
    s.version = "3.2.0"; s.platform = pm_tiny::daemon_info_platform::linux_os;
    s.pid = 123; s.uptime_ms = 456; s.run_mode = pm_tiny::daemon_run_mode::daemon;
    s.config_file = "/etc/pm_tiny.yaml"; s.config_loaded = true; s.home_dir = "/run/pm";
    s.pid_file = "/run/pm/pm_tiny.pid"; s.program_config_file = "/run/pm/prog.yaml";
    s.app_environ_dir = "/run/pm/environ"; s.app_log_dir = "/run/pm/logs";
    s.daemon_log_file = "/run/pm/pm_tiny.log"; s.uds_address = "/run/pm/pm.sock";
    s.allowed_uids = {0, 1000}; s.allowed_gids = {1000};
    s.requested_process_tree_mode = "auto"; s.effective_process_tree_mode = "cgroup";
    s.cgroup_root = "/sys/fs/cgroup/pm_tiny"; s.subreaper_enabled = true;
    s.log_level = "debug"; s.log_max_size_kb = 4096; s.log_archive_count = 3;
    s.log_sink = pm_tiny::daemon_log_sink::file; s.pty = true; s.switch_user = true;
    s.oom_adjust = true; s.failure_action = true; s.process_tree_backends = {"cgroup_v2", "process_group"};
    s.sources["home_dir"] = pm_tiny::daemon_config_source::environment;
    return s;
}
}

int main() {
    try {
        auto input = fixture();
        pm_tiny::frame_t frame;
        pm_tiny::append_daemon_info(frame, input);
        pm_tiny::iframe_stream stream(frame);
        const auto output = pm_tiny::read_daemon_info(stream);
        expect(output.version == "3.2.0" && output.allowed_uids.size() == 2, "round trip failed");
        expect(output.sources.at("home_dir") == pm_tiny::daemon_config_source::environment,
               "source round trip failed");

        const auto json = nlohmann::json::parse(pm_tiny::cli::render_daemon_info(output, true));
        expect(json.at("schema_version") == 1, "JSON schema missing");
        expect(json.at("identity").at("pid").is_number_integer(), "PID lost numeric type");
        expect(json.at("ipc").at("allowed_uids").at("value").is_array(), "UIDs lost array type");
        expect(json.at("config").at("home_dir").at("source") == "environment", "JSON source missing");
        const auto text = pm_tiny::cli::render_daemon_info(output, false);
        expect(text.find("Configuration") != std::string::npos && text.find("field") != std::string::npos,
               "text groups missing");
        expect(text.find("log_degraded:") != std::string::npos &&
               text.find("process_tree_degraded:") != std::string::npos,
               "text degraded fields should be unambiguous");
        expect(text.find("\n  degraded:") == std::string::npos,
               "text should not expose ambiguous degraded fields");

        auto invalid_schema = frame;
        invalid_schema[3] = 2;
        expect_decode_error([&]() { pm_tiny::iframe_stream s(invalid_schema); (void)pm_tiny::read_daemon_info(s); },
                              "unknown schema accepted");
        auto invalid_enum = frame;
        const std::size_t platform_offset = 4U + 4U + input.version.size() + 4U;
        invalid_enum[platform_offset] = 9;
        expect_decode_error([&]() { pm_tiny::iframe_stream s(invalid_enum); (void)pm_tiny::read_daemon_info(s); },
                              "invalid enum accepted");
        auto truncated = frame;
        truncated.pop_back();
        expect_decode_error([&]() { pm_tiny::iframe_stream s(truncated); (void)pm_tiny::read_daemon_info(s); },
                              "truncated payload accepted");
        auto trailing = frame;
        trailing.push_back(0);
        expect_decode_error([&]() { pm_tiny::iframe_stream s(trailing); (void)pm_tiny::read_daemon_info(s); },
                              "trailing payload accepted");

        const std::string key = "home_dir";
        const std::size_t record_size = 4U + key.size() + 1U;
        auto duplicate = frame;
        const std::size_t count_offset = duplicate.size() - record_size - 4U;
        duplicate[count_offset + 3] = 2;
        duplicate.insert(duplicate.end(), frame.end() - static_cast<std::ptrdiff_t>(record_size), frame.end());
        expect_decode_error([&]() { pm_tiny::iframe_stream s(duplicate); (void)pm_tiny::read_daemon_info(s); },
                              "duplicate source accepted");
        auto invalid_count = frame;
        invalid_count[count_offset + 2] = 1;
        invalid_count[count_offset + 3] = 1;
        expect_decode_error([&]() { pm_tiny::iframe_stream s(invalid_count); (void)pm_tiny::read_daemon_info(s); },
                              "invalid source count accepted");
        auto invalid_source = frame;
        invalid_source.back() = 9;
        expect_decode_error([&]() { pm_tiny::iframe_stream s(invalid_source); (void)pm_tiny::read_daemon_info(s); },
                              "invalid source enum accepted");
        auto unknown_key = frame;
        const std::size_t key_offset = count_offset + 8U;
        const std::string replacement = "bad_key_";
        std::copy(replacement.begin(), replacement.end(), unknown_key.begin() + static_cast<std::ptrdiff_t>(key_offset));
        expect_decode_error([&]() { pm_tiny::iframe_stream s(unknown_key); (void)pm_tiny::read_daemon_info(s); },
                              "unknown source key accepted");
        auto too_many = input;
        too_many.process_tree_backends.assign(pm_tiny::daemon_info_max_items + 1U, "x");
        expect_decode_error([&]() { pm_tiny::frame_t f; pm_tiny::append_daemon_info(f, too_many); },
                              "oversized item count accepted");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
