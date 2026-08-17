#pragma once

#include <string>
#include <map>
#include <vector>

namespace pm_tiny {

constexpr char windows_default_pipe_sddl[] =
    "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)";

enum class daemon_platform {
    posix,
    windows
};

enum class daemon_config_source {
    default_value = 0,
    config_file = 1,
    environment = 2,
    command_line = 3,
    derived = 4,
};

const char *daemon_config_source_name(daemon_config_source source);

struct daemon_cli_options {
    std::string config_path;
    std::string home_dir;
    std::string log_level;
    long log_max_size_kb = 0;
    int log_archive_count = -1;
    bool config_explicit = false;
    bool home_explicit = false;
    bool log_level_explicit = false;
    bool log_max_size_explicit = false;
    bool log_archive_count_explicit = false;
    bool daemonize = false;
    bool service = false;
    std::string service_name = "pm_tiny";
    bool service_name_explicit = false;
    std::string pipe_name;
    std::string pipe_sddl;
    bool help = false;
    bool version = false;
};

struct daemon_argument_result {
    bool success = false;
    daemon_cli_options options;
    std::string error;
};

struct daemon_config {
    std::string config_path;
    bool config_loaded = false;
    std::string home_dir;
    std::string lock_file;
    std::string log_file;
    std::string program_config_file;
    std::string app_log_dir;
    std::string app_environ_dir;
    std::string log_level = "info";
    int log_max_size_kb = 4096;
    int log_archive_count = 3;

    std::string socket_file;
    bool uds_abstract_namespace = false;
    std::string process_tree_mode = "auto";
    std::string cgroup_root;
    std::vector<unsigned int> allowed_uids;
    std::vector<unsigned int> allowed_gids;

    std::string pipe_name = "\\\\.\\pipe\\pm_tiny";
    std::string pipe_sddl = windows_default_pipe_sddl;
    std::map<std::string, daemon_config_source> sources;

    daemon_config_source source_of(const std::string &field) const;
};

struct daemon_config_result {
    bool success = false;
    daemon_config config;
    std::string error;
};

daemon_argument_result parse_daemon_arguments(const std::vector<std::string> &arguments,
                                              daemon_platform platform);
daemon_argument_result parse_daemon_arguments(int argc, char *argv[], daemon_platform platform);

daemon_config_result resolve_daemon_config(const daemon_cli_options &options,
                                           daemon_platform platform);

std::string daemon_usage(const std::string &program, daemon_platform platform);
std::string daemon_environment(const std::string &name);
bool set_daemon_environment(const std::string &name, const std::string &value,
                            std::string &error);

} // namespace pm_tiny
