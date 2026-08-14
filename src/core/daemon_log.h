#ifndef PM_TINY_DAEMON_LOG_H
#define PM_TINY_DAEMON_LOG_H

#include <cerrno>
#include <cstddef>
#include <string>

namespace pm_tiny {

enum class daemon_log_level_t {
    debug = 0,
    info = 1,
    warn = 2,
    error = 3,
    fatal = 4,
};

struct daemon_log_config_t {
    std::string path;
    std::size_t max_size_bytes = 4U * 1024U * 1024U;
    int archive_count = 3;
    bool mirror_console = true;
    daemon_log_level_t minimum_level = daemon_log_level_t::info;
};

struct daemon_log_snapshot_t {
    std::string level = "info";
    std::size_t max_size_bytes = 4U * 1024U * 1024U;
    int archive_count = 3;
    bool mirror_console = true;
    std::string sink = "console";
    bool degraded = false;
    std::string last_error;
};

const char *daemon_log_level_name(daemon_log_level_t level);
bool parse_daemon_log_level(const std::string &value, daemon_log_level_t &level);

bool configure_daemon_log(const daemon_log_config_t &config, std::string &error);
void reset_daemon_log();
daemon_log_snapshot_t daemon_log_snapshot();

void daemon_log_message(daemon_log_level_t level, const std::string &message);
void daemon_log_printf(daemon_log_level_t level, const char *file, int line,
                       const char *function, const char *format, ...);
void daemon_log_errno(daemon_log_level_t level, const char *file, int line,
                      const char *function, int error_number, const char *format, ...);
void daemon_log_signal(int signal_number);

} // namespace pm_tiny

#define PM_TINY_DLOG_DEBUG(...) \
    ::pm_tiny::daemon_log_printf(::pm_tiny::daemon_log_level_t::debug, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define PM_TINY_DLOG_INFO(...) \
    ::pm_tiny::daemon_log_printf(::pm_tiny::daemon_log_level_t::info, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define PM_TINY_DLOG_WARN(...) \
    ::pm_tiny::daemon_log_printf(::pm_tiny::daemon_log_level_t::warn, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define PM_TINY_DLOG_ERROR(...) \
    ::pm_tiny::daemon_log_printf(::pm_tiny::daemon_log_level_t::error, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define PM_TINY_DLOG_ERROR_ERRNO(...) \
    ::pm_tiny::daemon_log_errno(::pm_tiny::daemon_log_level_t::error, __FILE__, __LINE__, __func__, errno, __VA_ARGS__)

#endif
