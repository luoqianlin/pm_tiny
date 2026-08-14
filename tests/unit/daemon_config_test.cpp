#include "daemon_config.h"
#include "pm_tiny.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

void expect(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void unset_environment(const char *name) {
#ifdef _WIN32
    SetEnvironmentVariableA(name, nullptr);
#else
    unsetenv(name);
#endif
}

struct environment_guard {
    explicit environment_guard(std::vector<std::string> names) : names_(std::move(names)) {
        for (const auto &name : names_) values_.push_back(pm_tiny::daemon_environment(name));
    }
    ~environment_guard() {
        for (std::size_t i = 0; i < names_.size(); ++i) {
            if (values_[i].empty()) unset_environment(names_[i].c_str());
            else {
                std::string ignored;
                pm_tiny::set_daemon_environment(names_[i], values_[i], ignored);
            }
        }
    }
    std::vector<std::string> names_;
    std::vector<std::string> values_;
};

std::string temporary_directory() {
#ifdef _WIN32
    char root[MAX_PATH]{};
    expect(GetTempPathA(MAX_PATH, root) > 0, "GetTempPath failed");
    const std::string path = std::string(root) + "pm_tiny_config_test_" +
                             std::to_string(GetCurrentProcessId());
    CreateDirectoryA(path.c_str(), nullptr);
    return path;
#else
    std::string pattern = "/tmp/pm_tiny_config_test_XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    expect(mkdtemp(buffer.data()) != nullptr, "mkdtemp failed");
    return buffer.data();
#endif
}

void test_argument_parser() {
    const auto parsed = pm_tiny::parse_daemon_arguments(
        {"pm_tiny", "--home", "custom", "--log-level", "debug",
         "--log-max-size-kb", "128", "--log-archive-count", "2", "--daemon"},
        pm_tiny::daemon_platform::posix);
    expect(parsed.success, parsed.error);
    expect(parsed.options.home_dir == "custom" && parsed.options.daemonize,
           "common/POSIX arguments were not parsed");
    const auto windows = pm_tiny::parse_daemon_arguments(
        {"pm_tiny", "--service", "--service-name", "test", "--pipe-name", "pipe"},
        pm_tiny::daemon_platform::windows);
    expect(windows.success && windows.options.service && windows.options.pipe_name == "pipe",
           "Windows arguments were not parsed");
    expect(!pm_tiny::parse_daemon_arguments({"pm_tiny", "--service"},
        pm_tiny::daemon_platform::posix).success, "POSIX accepted Windows option");
    expect(!pm_tiny::parse_daemon_arguments({"pm_tiny", "--log-max-size-kb", "12x"},
        pm_tiny::daemon_platform::windows).success, "invalid integer was accepted");
}

void test_precedence_and_paths() {
    environment_guard guard({PM_TINY_HOME, PM_TINY_LOG_FILE, PM_TINY_LOG_LEVEL,
                             PM_TINY_LOG_MAX_SIZE_KB, PM_TINY_LOG_ARCHIVE_COUNT,
                             PM_TINY_PIPE_NAME, PM_TINY_PIPE_SDDL});
    const std::string directory = temporary_directory();
    const std::string config_path = directory + "/pm_tiny.yaml";
    std::ofstream config(config_path);
    config << "pm_tiny_home_dir: config-home\n"
              "pm_tiny_log_file: config.log\n"
              "pm_tiny_prog_cfg_file: prog.yaml\n"
              "pm_tiny_app_log_dir: logs\n"
              "pm_tiny_app_environ_dir: environ\n"
              "pm_tiny_log_level: warn\n"
              "pm_tiny_log_max_size_kb: 512\n"
              "pm_tiny_log_archive_count: 4\n"
              "pm_tiny_pipe_name: '\\\\.\\pipe\\from_config'\n"
              "pm_tiny_pipe_sddl: test-sddl\n";
    config.close();
    std::string error;
    expect(pm_tiny::set_daemon_environment(PM_TINY_HOME, directory + "/env-home", error), error);
    expect(pm_tiny::set_daemon_environment(PM_TINY_LOG_FILE, directory + "/env.log", error), error);
    expect(pm_tiny::set_daemon_environment(PM_TINY_LOG_LEVEL, "error", error), error);
    expect(pm_tiny::set_daemon_environment(PM_TINY_PIPE_NAME, "pipe-from-env", error), error);
    pm_tiny::daemon_cli_options options;
    options.config_path = config_path;
    options.config_explicit = true;
    options.home_dir = directory + "/cli-home";
    options.home_explicit = true;
    options.log_level = "debug";
    options.log_level_explicit = true;
    options.pipe_name = "pipe-from-cli";
    const auto result = pm_tiny::resolve_daemon_config(options, pm_tiny::daemon_platform::windows);
    expect(result.success, result.error);
    expect(result.config.home_dir == directory + "/cli-home", "CLI home did not win");
    expect(result.config.log_file == directory + "/env.log", "environment log path did not win");
    expect(result.config.log_level == "debug", "CLI log level did not win");
    expect(result.config.pipe_name == "pipe-from-cli", "CLI pipe did not win");
    expect(result.config.source_of("config_path") == pm_tiny::daemon_config_source::command_line,
           "config path source is wrong");
    expect(result.config.source_of("home_dir") == pm_tiny::daemon_config_source::command_line,
           "CLI home source is wrong");
    expect(result.config.source_of("log_file") == pm_tiny::daemon_config_source::environment,
           "environment log source is wrong");
    expect(result.config.source_of("program_config_file") == pm_tiny::daemon_config_source::config_file,
           "relative YAML path source changed");
    expect(result.config.source_of("lock_file") == pm_tiny::daemon_config_source::derived,
           "PID file should be derived");
    auto normalized_program = result.config.program_config_file;
    auto normalized_environ = result.config.app_environ_dir;
    auto normalized_directory = directory;
    for (auto &character : normalized_program) if (character == '\\') character = '/';
    for (auto &character : normalized_environ) if (character == '\\') character = '/';
    for (auto &character : normalized_directory) if (character == '\\') character = '/';
    expect(normalized_program == normalized_directory + "/prog.yaml",
           "relative config path was not resolved against daemon config");
    expect(normalized_environ == normalized_directory + "/environ",
           "relative environment directory was not resolved");
}

void test_invalid_configuration() {
    environment_guard guard({PM_TINY_LOG_MAX_SIZE_KB});
    std::string error;
    expect(pm_tiny::set_daemon_environment(PM_TINY_LOG_MAX_SIZE_KB, "invalid", error), error);
    const auto result = pm_tiny::resolve_daemon_config({}, pm_tiny::daemon_platform::windows);
    expect(!result.success && result.error.find(PM_TINY_LOG_MAX_SIZE_KB) != std::string::npos,
           "invalid environment value did not fail");
}

} // namespace

int main() {
    try {
        test_argument_parser();
        test_precedence_and_paths();
        test_invalid_configuration();
        std::cout << "daemon config tests passed\n";
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
