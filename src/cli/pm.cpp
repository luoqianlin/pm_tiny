#include "cli_command.h"
#include "connection_error.h"
#include "pm_funcs.h"
#include "pm_sys.h"
#include "pm_tiny_helper.h"
#include "session.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <pwd.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

volatile sig_atomic_t pm_is_stop = 0;

namespace {

std::unique_ptr<pm_tiny::session_t> connect_to_daemon() {
    const auto config = pm_tiny::get_pm_tiny_config();
    const auto &socket_path = config->pm_tiny_sock_file;
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("client socket error");
        return nullptr;
    }

    sockaddr_un endpoint{};
    endpoint.sun_family = AF_UNIX;
    socklen_t length = 0;
    if (config->uds_abstract_namespace) {
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
        error_info.transport = config->uds_abstract_namespace
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
        return EXIT_FAILURE;
    }
    const auto &command = parsed.command;
    if (command.kind == pm_tiny::cli::command_kind::help) {
        std::fputs(pm_tiny::cli::command_usage(argc > 0 ? argv[0] : "pm", true).c_str(), stdout);
        return EXIT_SUCCESS;
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
            display_proc_infos(*session, options);
            break;
        }
        case pm_tiny::cli::command_kind::graph: {
            auto options = command.graph_options;
            options.stdout_is_tty = pm_tiny::cli::stdout_supports_color();
            success = display_dependency_graph(*session, options);
            break;
        }
        case pm_tiny::cli::command_kind::start: {
            progcfg_t config;
            config.name = command.start.name;
            config.command = command.start.command;
            config.kill_timeout_sec = command.start.kill_timeout_sec;
            config.run_as = command.start.run_as;
            if (config.run_as.empty()) {
                if (const auto *password = getpwuid(getuid())) config.run_as = password->pw_name;
            }
            config.env_vars = command.start.env_vars;
            config.depends_on = command.start.depends_on;
            config.start_timeout = command.start.start_timeout;
            config.failure_action = command.start.failure_action;
            config.daemon = command.start.daemon ? 1 : 0;
            config.heartbeat_timeout = command.start.heartbeat_timeout;
            config.oom_score_adj = command.start.oom_score_adj;
            config.pty = command.start.pty;
            start_proc(*session, config, command.start.show_log);
            break;
        }
        case pm_tiny::cli::command_kind::stop: stop_proc(*session, command.name); break;
        case pm_tiny::cli::command_kind::restart:
            restart_prog(*session, command.name, command.show_log);
            break;
        case pm_tiny::cli::command_kind::remove: delete_prog(*session, command.name); break;
        case pm_tiny::cli::command_kind::save: save_proc(*session); break;
        case pm_tiny::cli::command_kind::log: show_prog_log(*session, command.name); break;
        case pm_tiny::cli::command_kind::inspect: inspect_proc(*session, command.name); break;
        case pm_tiny::cli::command_kind::reload: pm_tiny_reload(*session, 1); break;
        case pm_tiny::cli::command_kind::quit: success = pm_tiny_quit(*session); break;
        case pm_tiny::cli::command_kind::version: show_version(*session); break;
        case pm_tiny::cli::command_kind::help: break;
    }
    session->close();
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
