#ifndef PM_TINY_LOGGING_H
#define PM_TINY_LOGGING_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pm_tiny {

class iframe_stream;

enum class log_mode_t : std::int32_t {
    split = 0,
    combined = 1,
};

const char *log_mode_name(log_mode_t mode);
bool parse_log_mode(const std::string &value, log_mode_t &mode);
std::vector<std::string> derive_log_file_names(const std::string &program_name,
                                               log_mode_t mode,
                                               const std::string &configured_name);
std::vector<std::string> derive_log_paths(const std::string &directory,
                                          const std::string &program_name,
                                          log_mode_t mode,
                                          const std::string &configured_name);
std::string format_program_exit_event(const std::string &name,
                                      std::int64_t pid,
                                      const std::string &reason,
                                      std::int32_t code);

enum class log_request_mode : std::int32_t {
    live = 0,
    history = 1,
};

struct program_log_request {
    std::string name;
    log_request_mode mode = log_request_mode::live;
};

struct program_log_response {
    log_request_mode mode = log_request_mode::live;
    std::uint64_t generation = 0;
    std::int64_t last_pid = -1;
    std::int64_t last_exit_time_unix_ms = 0;
    std::string exit_reason;
    std::int32_t exit_code = 0;
};

void append_program_log_request(std::vector<std::uint8_t> &payload,
                                const program_log_request &request);
program_log_request read_program_log_request(const std::vector<std::uint8_t> &payload);
void append_program_log_response(std::vector<std::uint8_t> &payload,
                                 const program_log_response &response);
program_log_response read_program_log_response(iframe_stream &stream);
std::int64_t current_unix_time_millis();
std::string format_program_log_history_header(const std::string &name,
                                              const program_log_response &response);

class bounded_log_tail {
public:
    explicit bounded_log_tail(std::size_t capacity = 64U * 1024U);

    void clear();
    void append(const char *data, std::size_t size);
    std::string snapshot() const;
    std::string read(std::uint64_t offset, std::size_t max_size) const;
    std::uint64_t total_bytes() const { return total_bytes_; }
    std::uint64_t begin_offset() const { return total_bytes_ - size_; }
    std::size_t size() const { return size_; }

private:
    std::vector<char> storage_;
    std::size_t begin_ = 0;
    std::size_t size_ = 0;
    std::uint64_t total_bytes_ = 0;
};

struct log_sink_health {
    bool degraded = false;
    std::uint64_t dropped_bytes = 0;
    std::string last_error;
    std::int64_t retry_due_ms = 0;
    std::int64_t retry_delay_ms = 1000;

    void reset();
    bool retry_ready(std::int64_t now_ms) const;
    void record_failure(std::int64_t now_ms, std::uint64_t dropped,
                        const std::string &error);
    void record_recovery();
};

} // namespace pm_tiny

#endif
