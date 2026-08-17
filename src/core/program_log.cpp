#include "program_log.h"
#include "frame_stream.hpp"
#include "protocol_v3.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>

namespace pm_tiny {
namespace {
constexpr std::int32_t program_log_request_schema_version = 1;
constexpr std::int32_t program_log_response_schema_version = 1;

log_request_mode checked_log_request_mode(std::int32_t value) {
    if (value < static_cast<std::int32_t>(log_request_mode::live) ||
        value > static_cast<std::int32_t>(log_request_mode::history))
        throw protocol_error("invalid program-log mode");
    return static_cast<log_request_mode>(value);
}
} // namespace

const char *log_mode_name(log_mode_t mode) {
    switch (mode) {
        case log_mode_t::split: return "split";
        case log_mode_t::combined: return "combined";
    }
    return "split";
}

bool parse_log_mode(const std::string &value, log_mode_t &mode) {
    if (value == "split") mode = log_mode_t::split;
    else if (value == "combined") mode = log_mode_t::combined;
    else return false;
    return true;
}

std::vector<std::string> derive_log_file_names(const std::string &program_name,
                                               log_mode_t mode,
                                               const std::string &configured_name) {
    const std::string base = configured_name.empty() ? program_name + ".log" : configured_name;
    if (mode == log_mode_t::combined) return {base};

    const auto slash = base.find_last_of("/\\");
    const auto dot = base.find_last_of('.');
    const bool has_extension = dot != std::string::npos && dot != 0 &&
                               (slash == std::string::npos || dot > slash + 1);
    const std::string stem = has_extension ? base.substr(0, dot) : base;
    const std::string extension = has_extension ? base.substr(dot) : std::string();
    return {stem + "_stdout" + extension, stem + "_stderr" + extension};
}

std::vector<std::string> derive_log_paths(const std::string &directory,
                                          const std::string &program_name,
                                          log_mode_t mode,
                                          const std::string &configured_name) {
    auto names = derive_log_file_names(program_name, mode, configured_name);
    if (directory.empty()) return names;
    const char last = directory.back();
    const std::string separator = last == '/' || last == '\\' ? "" : "/";
    for (auto &name : names) name = directory + separator + name;
    return names;
}

std::string format_program_exit_event(const std::string &name,
                                      std::int64_t pid,
                                      const std::string &reason,
                                      std::int32_t code) {
    std::ostringstream output;
    output << "[pm_tiny] process `" << name << "` exited: pid=" << pid
           << " reason=" << reason << " code=" << code << "\n";
    return output.str();
}

void append_program_log_request(std::vector<std::uint8_t> &payload,
                                const program_log_request &request) {
    fappend_value(payload, request.name);
    fappend_value<std::int32_t>(payload, program_log_request_schema_version);
    fappend_value<std::int32_t>(payload, static_cast<std::int32_t>(request.mode));
}

program_log_request read_program_log_request(const std::vector<std::uint8_t> &payload) {
    iframe_stream stream(payload);
    program_log_request request;
    stream >> request.name;
    if (request.name.empty()) throw protocol_error("program-log name is empty");
    if (stream.remaining_size() == 0) return request;
    std::int32_t schema = 0, mode = 0;
    stream >> schema >> mode;
    if (schema != program_log_request_schema_version)
        throw protocol_error("unsupported program-log request schema");
    request.mode = checked_log_request_mode(mode);
    if (stream.remaining_size() != 0) throw protocol_error("trailing program-log request data");
    return request;
}

void append_program_log_response(std::vector<std::uint8_t> &payload,
                                 const program_log_response &response) {
    fappend_value<std::int32_t>(payload, program_log_response_schema_version);
    fappend_value<std::int32_t>(payload, static_cast<std::int32_t>(response.mode));
    fappend_value<std::uint64_t>(payload, response.generation);
    fappend_value<std::int64_t>(payload, response.last_pid);
    fappend_value<std::int64_t>(payload, response.last_exit_time_unix_ms);
    fappend_value(payload, response.exit_reason);
    fappend_value<std::int32_t>(payload, response.exit_code);
}

program_log_response read_program_log_response(iframe_stream &stream) {
    std::int32_t schema = 0, mode = 0;
    stream >> schema >> mode;
    if (schema != program_log_response_schema_version)
        throw protocol_error("unsupported program-log response schema");
    program_log_response response;
    response.mode = checked_log_request_mode(mode);
    stream >> response.generation >> response.last_pid >> response.last_exit_time_unix_ms
           >> response.exit_reason >> response.exit_code;
    if (response.generation == 0) throw protocol_error("invalid program-log generation");
    if (response.mode == log_request_mode::history &&
        (response.last_pid <= 0 || response.last_exit_time_unix_ms <= 0 || response.exit_reason.empty()))
        throw protocol_error("incomplete program-log history metadata");
    return response;
}

std::int64_t current_unix_time_millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string format_program_log_history_header(const std::string &name,
                                              const program_log_response &response) {
    const std::time_t seconds = static_cast<std::time_t>(response.last_exit_time_unix_ms / 1000);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char timestamp[32]{};
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    std::ostringstream output;
    output << "[pm_tiny] showing cached log for stopped process `" << name
           << "`; generation=" << response.generation << " last_exit=" << timestamp
           << " pid=" << response.last_pid << " reason=" << response.exit_reason
           << " code=" << response.exit_code << "\n";
    return output.str();
}

bounded_log_tail::bounded_log_tail(std::size_t capacity) : storage_(capacity) {}

void bounded_log_tail::clear() {
    begin_ = 0;
    size_ = 0;
    total_bytes_ = 0;
}

void bounded_log_tail::append(const char *data, std::size_t size) {
    if (data == nullptr || size == 0) return;
    total_bytes_ += size;
    if (storage_.empty()) return;
    if (size >= storage_.size()) {
        data += size - storage_.size();
        size = storage_.size();
        std::copy(data, data + size, storage_.begin());
        begin_ = 0;
        size_ = size;
        return;
    }
    const std::size_t overflow = size_ + size > storage_.size() ? size_ + size - storage_.size() : 0;
    begin_ = (begin_ + overflow) % storage_.size();
    size_ -= overflow;
    for (std::size_t i = 0; i < size; ++i) {
        storage_[(begin_ + size_ + i) % storage_.size()] = data[i];
    }
    size_ += size;
}

std::string bounded_log_tail::snapshot() const {
    return read(begin_offset(), size_);
}

std::string bounded_log_tail::read(std::uint64_t offset, std::size_t max_size) const {
    if (offset < begin_offset()) offset = begin_offset();
    if (offset >= total_bytes_ || max_size == 0 || storage_.empty()) return {};
    const std::size_t local = static_cast<std::size_t>(offset - begin_offset());
    const std::size_t count = std::min(max_size, size_ - local);
    std::string result(count, '\0');
    for (std::size_t i = 0; i < count; ++i)
        result[i] = storage_[(begin_ + local + i) % storage_.size()];
    return result;
}

void log_sink_health::reset() {
    degraded = false;
    dropped_bytes = 0;
    last_error.clear();
    retry_due_ms = 0;
    retry_delay_ms = 1000;
}

bool log_sink_health::retry_ready(std::int64_t now_ms) const {
    return degraded && now_ms >= retry_due_ms;
}

void log_sink_health::record_failure(std::int64_t now_ms, std::uint64_t dropped,
                                     const std::string &error) {
    degraded = true;
    dropped_bytes += dropped;
    last_error = error;
    retry_due_ms = now_ms + retry_delay_ms;
    retry_delay_ms = std::min<std::int64_t>(retry_delay_ms * 2, 60000);
}

void log_sink_health::record_recovery() {
    degraded = false;
    retry_due_ms = 0;
    retry_delay_ms = 1000;
}

} // namespace pm_tiny
