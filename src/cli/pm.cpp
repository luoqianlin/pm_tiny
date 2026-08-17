//
// Created by luo on 2021/10/6.
//
#include "cli_command.h"
#include "posix_privilege_wrapper.h"
#include "connection_error.h"
#include "pm_funcs.h"
#include "pm_sys.h"
#include "daemon_config.h"
#include "session.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <memory>
#include <pwd.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

volatile sig_atomic_t pm_is_stop = 0;

namespace {

std::string absolute_existing_path(const std::string &path, const char *kind) {
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved) == nullptr) {
        throw std::runtime_error(std::string("resolve ") + kind + " `" + path + "`: " + std::strerror(errno));
    }
    return resolved;
}

bool executable_file(const std::string &path) {
    struct stat info{};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && access(path.c_str(), X_OK) == 0;
}

std::string resolve_start_executable(const std::string &input, const std::string &cwd) {
    if (input.empty()) throw std::runtime_error("resolve executable: empty path");
    if (input.find('/') != std::string::npos) {
        const std::string candidate = input.front() == '/' ? input : cwd + "/" + input;
        const auto resolved = absolute_existing_path(candidate, "executable");
        if (!executable_file(resolved))
            throw std::runtime_error("resolve executable `" + input + "`: file is not executable");
        return resolved;
    }

    char cwd_resolved[PATH_MAX];
    if (realpath((cwd + "/" + input).c_str(), cwd_resolved) != nullptr && executable_file(cwd_resolved))
        return cwd_resolved;

    const char *path_env = std::getenv("PATH");
    std::string path_list = path_env == nullptr ? std::string() : path_env;
    std::size_t begin = 0;
    while (begin <= path_list.size()) {
        const auto end = path_list.find(':', begin);
        const auto directory = path_list.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        const std::string candidate = (directory.empty() ? std::string(".") : directory) + "/" + input;
        char resolved[PATH_MAX];
        if (realpath(candidate.c_str(), resolved) != nullptr && executable_file(resolved)) return resolved;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    throw std::runtime_error("resolve executable not found: " + input);
}

std::unique_ptr<pm_tiny::session_t> connect_to_daemon() {
    const pm_tiny::daemon_cli_options options;
    const auto resolved = pm_tiny::resolve_daemon_config(options, pm_tiny::daemon_platform::posix);
    if (!resolved.success) {
        std::fprintf(stderr, "pm: %s\n", resolved.error.c_str());
        return nullptr;
    }
    const auto &socket_path = resolved.config.socket_file;
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("client socket error");
        return nullptr;
    }

    sockaddr_un endpoint{};
    endpoint.sun_family = AF_UNIX;
    socklen_t length = 0;
    if (resolved.config.uds_abstract_namespace) {
        if (socket_path.size() >= sizeof(endpoint.sun_path) - 1) {
            std::fprintf(stderr, "pm: abstract socket name is too long\n");
            close(fd);
            return nullptr;
        }
        endpoint.sun_path[0] = '\0';
        std::memcpy(endpoint.sun_path + 1, socket_path.data(), socket_path.size());
        length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + socket_path.size() + 1);
    } else {
        if (socket_path.size() >= sizeof(endpoint.sun_path)) {
            std::fprintf(stderr, "pm: socket path is too long\n");
            close(fd);
            return nullptr;
        }
        std::memcpy(endpoint.sun_path, socket_path.c_str(), socket_path.size() + 1);
        length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + socket_path.size());
    }
    if (connect(fd, reinterpret_cast<sockaddr *>(&endpoint), length) < 0) {
        const int connect_errno = errno;
        close(fd);
        pm_tiny::cli::connection_error_info error_info;
        error_info.endpoint = socket_path;
        error_info.transport = resolved.config.uds_abstract_namespace
            ? pm_tiny::cli::connection_transport::unix_abstract
            : pm_tiny::cli::connection_transport::unix_filesystem;
        error_info.reason = std::strerror(connect_errno);
        error_info.code_name = "errno";
        error_info.code = static_cast<unsigned long>(connect_errno);
        error_info.category = pm_tiny::cli::classify_posix_connection_error(connect_errno);
        std::fputs(pm_tiny::cli::format_connection_error(error_info).c_str(), stderr);
        return nullptr;
    }
    if (pm_tiny::set_nonblock(fd) == -1) {
        perror("fcntl O_NONBLOCK");
        close(fd);
        return nullptr;
    }
    return std::make_unique<pm_tiny::session_t>(fd, 0);
}

void configure_signals() {
    if (pm_tiny::set_sigaction(SIGPIPE, SIG_IGN) == -1) {
        perror("sigaction");
        std::exit(EXIT_FAILURE);
    }
    auto sigint_handler = [](int) { pm_is_stop = 1; };
    if (pm_tiny::set_sigaction(SIGINT, sigint_handler) == -1) {
        perror("sigaction");
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main(int argc, char *argv[]) {
    const auto parsed = pm_tiny::cli::parse_command_line(argc, argv);
    if (!parsed.success) {
        std::fprintf(stderr, "pm: %s\n\n%s", parsed.error.c_str(),
                     pm_tiny::cli::command_usage(argc > 0 ? argv[0] : "pm", true).c_str());
        return 2;
    }
    const auto &command = parsed.command;
    if (command.kind == pm_tiny::cli::command_kind::help) {
        std::fputs(pm_tiny::cli::command_usage(argc > 0 ? argv[0] : "pm", true).c_str(), stdout);
        return EXIT_SUCCESS;
    }
    if (command.kind == pm_tiny::cli::command_kind::start && command.start.create &&
        pm_tiny::cli::is_privilege_wrapper_executable(command.start.executable)) {
        std::fprintf(stderr,
                     "pm: warning: interactive privilege elevation is unsupported; "
                     "run pm_tiny as root and use --user instead of `%s`\n",
                     command.start.executable.c_str());
    }
    if (command.kind == pm_tiny::cli::command_kind::start && command.start.create &&
        !command.start.run_as.empty() && command.start.executable.find('/') == std::string::npos) {
        const passwd *target = getpwnam(command.start.run_as.c_str());
        if (target != nullptr && target->pw_uid != geteuid()) {
            std::fprintf(stderr, "pm: cross-user start requires executable containing `/`: `%s`\n",
                         command.start.executable.c_str());
            return EXIT_FAILURE;
        }
    }

    configure_signals();
    auto session = connect_to_daemon();
    if (!session) return EXIT_FAILURE;

    using namespace pm_funcs;
    bool success = true;
    switch (command.kind) {
        case pm_tiny::cli::command_kind::list: {
            auto options = command.list_options;
            options.terminal_width = pm_tiny::cli::stdout_terminal_width();
            options.stdout_is_tty = pm_tiny::cli::stdout_supports_color();
            success = display_proc_infos(*session, options);
            break;
        }
        case pm_tiny::cli::command_kind::graph: {
            auto options = command.graph_options;
            options.stdout_is_tty = pm_tiny::cli::stdout_supports_color();
            success = display_dependency_graph(*session, options);
            break;
        }
        case pm_tiny::cli::command_kind::start: {
            pm_tiny::start_request request;
            request.mode = command.start.create ? pm_tiny::start_mode::create : pm_tiny::start_mode::existing;
            request.name = command.start.name;
            request.show_log = command.start.show_log;
            if (command.start.create) {
                auto &config = request.config;
                config.name = command.start.name;
                config.cwd = command.start.cwd;
                if (config.cwd.empty()) {
                    char cwd[PATH_MAX];
                    if (getcwd(cwd, sizeof(cwd)) == nullptr) {
                        perror("getcwd");
                        success = false;
                        break;
                    }
                    config.cwd = cwd;
                }
                try {
                    config.cwd = absolute_existing_path(config.cwd, "cwd");
                    config.executable = resolve_start_executable(command.start.executable, config.cwd);
                } catch (const std::exception &error) {
                    std::fprintf(stderr, "pm: %s\n", error.what());
                    success = false;
                    break;
                }
                config.args = command.start.args;
                config.kill_timeout_s = command.start.kill_timeout_sec;
                config.run_as = command.start.run_as;
                config.env_vars = command.start.env;
                config.depends_on = command.start.depends_on;
                config.start_timeout = command.start.start_timeout;
                config.failure_action = command.start.failure_action;
                config.daemon = command.start.daemon;
                config.heartbeat_timeout = command.start.heartbeat_timeout;
                config.oom_score_adj = command.start.oom_score_adj;
                config.pty = command.start.pty;
                config.log_mode = command.start.log_mode_explicit
                                      ? command.start.log_mode
                                      : (config.pty ? pm_tiny::log_mode_t::combined : pm_tiny::log_mode_t::split);
                if (config.pty && config.log_mode == pm_tiny::log_mode_t::split) {
                    std::cerr << "PTY requires --log-mode combined" << std::endl;
                    success = false;
                    break;
                }
                config.log_dir = command.start.log_dir;
                config.log_file_name = command.start.log_file_name;
                config.log_max_size_kb = command.start.log_max_size_kb;
                config.log_archive_count = command.start.log_archive_count;
                config.restart_delay_ms = command.start.restart_delay_ms;
                config.restart_max_delay_ms = command.start.restart_max_delay_ms;
                config.restart_window_ms = command.start.restart_window_ms;
                config.restart_max_attempts = command.start.restart_max_attempts;
                config.restart_reset_after_ms = command.start.restart_reset_after_ms;
                for (char **env = ::environ; *env != nullptr; ++env) {
                    if (std::strncmp(*env, "PM_TINY_", 8) != 0) request.inherited_env.emplace_back(*env);
                }
            }
            success = start_proc(*session, request);
            break;
        }
        case pm_tiny::cli::command_kind::stop: success = stop_proc(*session, command.name, command.no_list); break;
        case pm_tiny::cli::command_kind::restart:
            success = restart_prog(*session, command.name, command.show_log, command.no_list);
            break;
        case pm_tiny::cli::command_kind::remove: success = delete_prog(*session, command.name, command.no_list); break;
        case pm_tiny::cli::command_kind::save: success = save_proc(*session); break;
        case pm_tiny::cli::command_kind::log:
            success = show_prog_log(*session, command.name, command.log_history);
            break;
        case pm_tiny::cli::command_kind::inspect: success = inspect_proc(*session, command.name); break;
        case pm_tiny::cli::command_kind::reload: success = pm_tiny_reload(*session, 1, command.no_list); break;
        case pm_tiny::cli::command_kind::quit: success = pm_tiny_quit(*session); break;
        case pm_tiny::cli::command_kind::version: success = show_version(*session); break;
        case pm_tiny::cli::command_kind::info: success = display_daemon_info(*session, command.info_json); break;
        case pm_tiny::cli::command_kind::help: break;
    }
    session->close();
    if (pm_is_stop) return 130;
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
