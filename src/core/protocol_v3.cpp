#include "protocol_v3.h"

#include "frame_stream.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace pm_tiny {
namespace {
constexpr std::size_t header_size = 16;
constexpr char magic[] = {'P', 'M', 'T', '3'};
constexpr std::int32_t prog_cfg_schema_version = 2;
constexpr std::int32_t start_schema_version = 2;
constexpr std::int32_t max_start_items = 100000;

void put16(std::uint8_t *p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8); p[1] = static_cast<std::uint8_t>(v);
}
void put32(std::uint8_t *p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24); p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8); p[3] = static_cast<std::uint8_t>(v);
}
std::uint16_t get16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
std::uint32_t get32(const std::uint8_t *p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
void validate_flags(std::uint8_t flags) {
    constexpr std::uint8_t allowed = protocol_flag_response | protocol_flag_error |
                                      protocol_flag_stream | protocol_flag_more;
    if ((flags & ~allowed) != 0 || ((flags & protocol_flag_error) && !(flags & protocol_flag_response)) ||
        ((flags & protocol_flag_more) && !(flags & protocol_flag_stream))) {
        throw protocol_error("invalid protocol flags");
    }
}
void append_bool(frame_t &frame, bool value) { fappend_value<std::int32_t>(frame, value ? 1 : 0); }
bool read_bool(iframe_stream &stream, const char *field) {
    std::int32_t value = 0;
    stream >> value;
    if (value != 0 && value != 1) throw protocol_error(field);
    return value != 0;
}
void append_strings(frame_t &frame, const std::vector<std::string> &values) {
    if (values.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        throw protocol_error("start list is too large");
    fappend_value<std::int32_t>(frame, static_cast<std::int32_t>(values.size()));
    for (const auto &value : values) fappend_value(frame, value);
}
std::vector<std::string> read_strings(iframe_stream &stream, const char *field) {
    std::int32_t count = 0;
    stream >> count;
    if (count < 0 || count > max_start_items) throw protocol_error(field);
    std::vector<std::string> values(static_cast<std::size_t>(count));
    for (auto &value : values) stream >> value;
    return values;
}
} // namespace

void append_prog_cfg(frame_t &frame, const prog_cfg_t &cfg) {
    fappend_value<std::int32_t>(frame, prog_cfg_schema_version);
    fappend_value(frame, cfg.name);
    fappend_value(frame, cfg.cwd);
    fappend_value(frame, cfg.executable);
    append_strings(frame, cfg.args);
    fappend_value<std::int32_t>(frame, cfg.kill_timeout_s);
    fappend_value(frame, cfg.run_as);
    append_strings(frame, cfg.env_vars);
    append_strings(frame, cfg.depends_on);
    fappend_value<std::int32_t>(frame, cfg.start_timeout);
    fappend_value<std::int32_t>(frame, static_cast<std::int32_t>(cfg.failure_action));
    append_bool(frame, cfg.daemon);
    fappend_value<std::int32_t>(frame, cfg.heartbeat_timeout);
    fappend_value<std::int32_t>(frame, cfg.oom_score_adj);
    append_bool(frame, cfg.pty);
    fappend_value<std::int32_t>(frame, static_cast<std::int32_t>(cfg.log_mode));
    fappend_value(frame, cfg.log_dir);
    fappend_value(frame, cfg.log_file_name);
    fappend_value<std::int32_t>(frame, cfg.log_max_size_kb);
    fappend_value<std::int32_t>(frame, cfg.log_archive_count);
    fappend_value<std::int32_t>(frame, cfg.restart_delay_ms);
    fappend_value<std::int32_t>(frame, cfg.restart_max_delay_ms);
    fappend_value<std::int32_t>(frame, cfg.restart_window_ms);
    fappend_value<std::int32_t>(frame, cfg.restart_max_attempts);
    fappend_value<std::int32_t>(frame, cfg.restart_reset_after_ms);
}
prog_cfg_t read_prog_cfg(iframe_stream &stream) {
    prog_cfg_t cfg;
    std::int32_t schema = 0;
    stream >> schema;
    if (schema != prog_cfg_schema_version) throw protocol_error("unsupported program-config schema");
    stream >> cfg.name >> cfg.cwd >> cfg.executable;
    cfg.args = read_strings(stream, "invalid start argv count");
    stream >> cfg.kill_timeout_s >> cfg.run_as;
    cfg.env_vars = read_strings(stream, "invalid explicit environment count");
    cfg.depends_on = read_strings(stream, "invalid dependency count");
    std::int32_t failure_action = 0;
    stream >> cfg.start_timeout >> failure_action;
    if (failure_action < static_cast<std::int32_t>(failure_action_t::SKIP) ||
        failure_action > static_cast<std::int32_t>(failure_action_t::REBOOT))
        throw protocol_error("invalid failure action");
    cfg.failure_action = static_cast<failure_action_t>(failure_action);
    cfg.daemon = read_bool(stream, "invalid daemon flag");
    stream >> cfg.heartbeat_timeout >> cfg.oom_score_adj;
    cfg.pty = read_bool(stream, "invalid PTY flag");
    std::int32_t log_mode = 0;
    stream >> log_mode >> cfg.log_dir >> cfg.log_file_name >> cfg.log_max_size_kb >> cfg.log_archive_count;
    if (log_mode < static_cast<std::int32_t>(log_mode_t::split) ||
        log_mode > static_cast<std::int32_t>(log_mode_t::combined))
        throw protocol_error("invalid log mode");
    cfg.log_mode = static_cast<log_mode_t>(log_mode);
    stream >> cfg.restart_delay_ms >> cfg.restart_max_delay_ms >> cfg.restart_window_ms
           >> cfg.restart_max_attempts >> cfg.restart_reset_after_ms;
    return cfg;
}

namespace {

void validate_start_config(const prog_cfg_t &cfg) {
    if (cfg.name.empty() || cfg.cwd.empty() || cfg.executable.empty())
        throw protocol_error("start definition requires name, cwd and executable");
    if (cfg.kill_timeout_s < 1) throw protocol_error("kill timeout must be positive");
    if (cfg.oom_score_adj < -1000 || cfg.oom_score_adj > 1000)
        throw protocol_error("oom score adjustment is out of range");
    if (cfg.restart_delay_ms < 0 || cfg.restart_max_delay_ms < cfg.restart_delay_ms ||
        cfg.restart_window_ms < 1 || cfg.restart_max_attempts < 0 || cfg.restart_reset_after_ms < 0)
        throw protocol_error("invalid restart policy");
    if (cfg.log_max_size_kb < 1 || cfg.log_max_size_kb > 1048576 ||
        cfg.log_archive_count < 0 || cfg.log_archive_count > 100)
        throw protocol_error("invalid log rotation configuration");
    if (cfg.pty && cfg.log_mode == log_mode_t::split)
        throw protocol_error("PTY requires combined log mode");
    for (const auto &env : cfg.env_vars) {
        if (env.empty() || env[0] == '=' || env.find('=') == std::string::npos)
            throw protocol_error("invalid explicit environment entry");
        if (env.rfind("PM_TINY_", 0) == 0)
            throw protocol_error("reserved PM_TINY environment override");
    }
}
} // namespace

std::vector<std::uint8_t> protocol_encode(const protocol_message &message) {
    if (message.payload.size() > protocol_v3_max_payload)
        throw protocol_error("protocol payload exceeds maximum size");
    validate_flags(message.flags);
    std::vector<std::uint8_t> out(header_size + message.payload.size());
    std::memcpy(out.data(), magic, sizeof(magic));
    out[4] = protocol_v3_version;
    out[5] = message.flags;
    put16(out.data() + 6, message.type);
    put32(out.data() + 8, message.request_id);
    put32(out.data() + 12, static_cast<std::uint32_t>(message.payload.size()));
    std::copy(message.payload.begin(), message.payload.end(), out.begin() + header_size);
    return out;
}

void protocol_decoder::feed(const std::uint8_t *data, std::size_t size) {
    if (size == 0) return;
    buffer_.insert(buffer_.end(), data, data + size);
    while (buffer_.size() >= header_size) {
        if (!std::equal(buffer_.begin(), buffer_.begin() + 4, magic))
            throw protocol_error("invalid protocol magic");
        if (buffer_[4] != protocol_v3_version) throw protocol_error("unsupported protocol version");
        validate_flags(buffer_[5]);
        const auto payload_size = get32(buffer_.data() + 12);
        if (payload_size > protocol_v3_max_payload)
            throw protocol_error("protocol payload exceeds maximum size");
        const std::size_t total = header_size + static_cast<std::size_t>(payload_size);
        if (buffer_.size() < total) return;
        protocol_message message;
        message.flags = buffer_[5];
        message.type = get16(buffer_.data() + 6);
        message.request_id = get32(buffer_.data() + 8);
        message.payload.assign(buffer_.begin() + header_size, buffer_.begin() + total);
        messages_.push_back(std::move(message));
        buffer_.erase(buffer_.begin(), buffer_.begin() + total);
    }
}

protocol_message protocol_decoder::pop() {
    if (messages_.empty()) throw protocol_error("no decoded protocol message");
    auto message = std::move(messages_.front());
    messages_.erase(messages_.begin());
    return message;
}

void append_start_request(std::vector<std::uint8_t> &payload, const start_request &request) {
    fappend_value<std::int32_t>(payload, start_schema_version);
    fappend_value<std::int32_t>(payload, static_cast<std::int32_t>(request.mode));
    fappend_value(payload, request.name);
    append_prog_cfg(payload, request.config);
    append_strings(payload, request.inherited_env);
    append_bool(payload, request.show_log);
}

start_request read_start_request(const std::vector<std::uint8_t> &payload) {
    iframe_stream stream(payload);
    std::int32_t schema = 0, mode = 0;
    stream >> schema >> mode;
    if (schema != start_schema_version) throw protocol_error("unsupported start-request schema");
    if (mode < 0 || mode > 1) throw protocol_error("invalid start mode");
    start_request request;
    request.mode = static_cast<start_mode>(mode);
    stream >> request.name;
    request.config = read_prog_cfg(stream);
    request.inherited_env = read_strings(stream, "invalid inherited environment count");
    request.show_log = read_bool(stream, "invalid log flag");
    if (stream.remaining_size() != 0) throw protocol_error("trailing start-request data");
    if (request.name.empty()) throw protocol_error("start name is empty");
    if (request.mode == start_mode::create) validate_start_config(request.config);
    return request;
}

void append_start_response(std::vector<std::uint8_t> &payload, const start_response &response) {
    fappend_value<std::int32_t>(payload, start_schema_version);
    fappend_value<std::int32_t>(payload, static_cast<std::int32_t>(response.result));
    fappend_value<std::int64_t>(payload, response.pid);
    fappend_value(payload, response.state);
    append_strings(payload, response.blocked_by);
    fappend_value(payload, response.message);
}

start_response read_start_response(const std::vector<std::uint8_t> &payload) {
    iframe_stream stream(payload);
    std::int32_t schema = 0, result_value = 0;
    stream >> schema >> result_value;
    if (schema != start_schema_version) throw protocol_error("unsupported start-response schema");
    if (result_value < 0 || result_value > 2) throw protocol_error("invalid start result");
    start_response response;
    response.result = static_cast<start_result>(result_value);
    stream >> response.pid >> response.state;
    response.blocked_by = read_strings(stream, "invalid blocker count");
    stream >> response.message;
    if (stream.remaining_size() != 0) throw protocol_error("trailing start-response data");
    return response;
}

} // namespace pm_tiny
