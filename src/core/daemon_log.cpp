#include "daemon_log.h"

#include "rotating_log_writer.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include "signal_util.h"
#include <unistd.h>
#endif

namespace pm_tiny {
namespace {

std::string format_message(const char *format, va_list arguments) {
    if (format == nullptr) return "invalid null log format";
    va_list copy;
    va_copy(copy, arguments);
    const int size = std::vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    if (size < 0) return "log message formatting failed";
    std::vector<char> buffer(static_cast<std::size_t>(size) + 1U, '\0');
    if (size > 0) std::vsnprintf(buffer.data(), buffer.size(), format, arguments);
    return std::string(buffer.data(), static_cast<std::size_t>(size));
}

const char *short_file_name(const char *path) {
    if (path == nullptr) return "unknown";
    const char *slash = std::strrchr(path, '/');
    const char *backslash = std::strrchr(path, '\\');
    const char *separator = slash == nullptr ? backslash :
                            backslash == nullptr ? slash : std::max(slash, backslash);
    return separator == nullptr ? path : separator + 1;
}

std::string timestamp_prefix(daemon_log_level_t level) {
    const auto now = std::chrono::system_clock::now();
    const auto epoch = now.time_since_epoch();
    const auto microseconds = static_cast<long>(
        std::chrono::duration_cast<std::chrono::microseconds>(epoch).count() % 1000000);
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char timestamp[32]{};
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);
    char prefix[96]{};
    std::snprintf(prefix, sizeof(prefix), "%s.%06ld [%s] [daemon] ", timestamp,
                  microseconds, daemon_log_level_name(level));
    return prefix;
}

void write_descriptor(int descriptor, const std::string &line) {
    const char *data = line.data();
    std::size_t remaining = line.size();
    while (remaining > 0) {
        const auto count =
#ifdef _WIN32
            ::_write(descriptor, data, static_cast<unsigned int>(remaining));
#else
            ::write(descriptor, data, remaining);
#endif
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        data += count;
        remaining -= static_cast<std::size_t>(count);
    }
}

class daemon_log_state {
public:
    bool configure(const daemon_log_config_t &config, std::string &error) {
        std::lock_guard<std::mutex> lock(mutex_);
        minimum_level_ = config.minimum_level;
        mirror_console_ = config.mirror_console;
        path_ = config.path;
        max_size_bytes_ = config.max_size_bytes;
        archive_count_ = config.archive_count;
        last_error_.clear();
        writer_.reset();
        if (config.path.empty()) {
            error.clear();
            return true;
        }
        std::unique_ptr<rotating_log_writer> writer(new rotating_log_writer(
            config.path, config.max_size_bytes, config.archive_count));
        if (!writer->open(error)) {
            last_error_ = error;
            write_descriptor(2, "[error] [daemon] daemon log open failed: " + error + "\n");
            return false;
        }
        writer_ = std::move(writer);
        error.clear();
        return true;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        writer_.reset();
        mirror_console_ = true;
        minimum_level_ = daemon_log_level_t::info;
        path_.clear();
        max_size_bytes_ = 4U * 1024U * 1024U;
        archive_count_ = 3;
        last_error_.clear();
    }

    void write(daemon_log_level_t level, std::string message) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (level < minimum_level_) return;
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
            message.pop_back();
        const std::string line = timestamp_prefix(level) + message + "\n";
        if (writer_) {
            std::string error;
            if (!writer_->append(line.data(), line.size(), error)) {
                writer_.reset();
                last_error_ = error;
                write_descriptor(2, "[error] [daemon] daemon log write failed: " + error + "\n");
            }
        }
        if (mirror_console_ || !writer_) {
            write_descriptor(level >= daemon_log_level_t::error ? 2 : 1, line);
        }
    }

    daemon_log_snapshot_t snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        daemon_log_snapshot_t result;
        result.level = daemon_log_level_name(minimum_level_);
        result.max_size_bytes = max_size_bytes_;
        result.archive_count = archive_count_;
        result.mirror_console = mirror_console_;
        result.sink = writer_ ? "file" : (path_.empty() ? "console" : "console_fallback");
        result.degraded = !last_error_.empty();
        result.last_error = last_error_;
        return result;
    }

private:
    std::mutex mutex_;
    std::unique_ptr<rotating_log_writer> writer_;
    bool mirror_console_ = true;
    daemon_log_level_t minimum_level_ = daemon_log_level_t::info;
    std::string path_;
    std::size_t max_size_bytes_ = 4U * 1024U * 1024U;
    int archive_count_ = 3;
    std::string last_error_;
};

daemon_log_state &log_state() {
    static daemon_log_state state;
    return state;
}

std::string source_message(const char *file, int line, const char *function,
                           const std::string &message) {
    return "[" + std::string(short_file_name(file)) + ":" + std::to_string(line) + " " +
           (function == nullptr ? "unknown" : function) + "] " + message;
}

} // namespace

const char *daemon_log_level_name(daemon_log_level_t level) {
    switch (level) {
        case daemon_log_level_t::debug: return "debug";
        case daemon_log_level_t::info: return "info";
        case daemon_log_level_t::warn: return "warn";
        case daemon_log_level_t::error: return "error";
        case daemon_log_level_t::fatal: return "fatal";
    }
    return "info";
}

bool parse_daemon_log_level(const std::string &value, daemon_log_level_t &level) {
    if (value == "debug") level = daemon_log_level_t::debug;
    else if (value == "info") level = daemon_log_level_t::info;
    else if (value == "warn") level = daemon_log_level_t::warn;
    else if (value == "error") level = daemon_log_level_t::error;
    else if (value == "fatal") level = daemon_log_level_t::fatal;
    else return false;
    return true;
}

bool configure_daemon_log(const daemon_log_config_t &config, std::string &error) {
    return log_state().configure(config, error);
}

void reset_daemon_log() {
    log_state().reset();
}

daemon_log_snapshot_t daemon_log_snapshot() {
    return log_state().snapshot();
}

void daemon_log_message(daemon_log_level_t level, const std::string &message) {
    log_state().write(level, message);
}

void daemon_log_printf(daemon_log_level_t level, const char *file, int line,
                       const char *function, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const std::string message = format_message(format, arguments);
    va_end(arguments);
    log_state().write(level, source_message(file, line, function, message));
}

void daemon_log_errno(daemon_log_level_t level, const char *file, int line,
                      const char *function, int error_number, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    std::string message = format_message(format, arguments);
    va_end(arguments);
    if (error_number != 0)
        message += ": " + std::error_code(error_number, std::generic_category()).message();
    log_state().write(level, source_message(file, line, function, message));
}

void daemon_log_signal(int signal_number) {
#ifdef _WIN32
    daemon_log_message(daemon_log_level_t::info,
                       "received signal " + std::to_string(signal_number));
#else
    char buffer[200]{};
    mgr::utils::signal::signal_log(signal_number, buffer);
    daemon_log_message(daemon_log_level_t::info, buffer);
#endif
}

} // namespace pm_tiny
