#ifndef PM_TINY_PROTOCOL_V3_H
#define PM_TINY_PROTOCOL_V3_H

#include "prog_cfg.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace pm_tiny {

class iframe_stream;

constexpr std::uint32_t protocol_v3_max_payload = 4u * 1024u * 1024u;
constexpr std::uint8_t protocol_v3_version = 3;

enum protocol_v3_flags : std::uint8_t {
    protocol_flag_response = 0x01,
    protocol_flag_error = 0x02,
    protocol_flag_stream = 0x04,
    protocol_flag_more = 0x08,
};

struct protocol_message {
    std::uint16_t type = 0;
    std::uint8_t flags = 0;
    std::uint32_t request_id = 0;
    std::vector<std::uint8_t> payload;
};

class protocol_error : public std::runtime_error {
public:
    explicit protocol_error(const char *message) : std::runtime_error(message) {}
    explicit protocol_error(const std::string &message) : std::runtime_error(message) {}
};

enum class start_mode : std::int32_t {
    existing = 0,
    create = 1,
};

enum class start_result : std::int32_t {
    started = 0,
    waiting = 1,
    blocked = 2,
};

struct start_request {
    start_mode mode = start_mode::existing;
    std::string name;
    prog_cfg_t config;
    std::vector<std::string> inherited_env;
    bool show_log = false;
};

struct start_response {
    start_result result = start_result::started;
    std::int64_t pid = -1;
    std::string state;
    std::vector<std::string> blocked_by;
    std::string message;
};

std::vector<std::uint8_t> protocol_encode(const protocol_message &message);
void append_prog_cfg(std::vector<std::uint8_t> &payload, const prog_cfg_t &config);
prog_cfg_t read_prog_cfg(iframe_stream &stream);
void append_start_request(std::vector<std::uint8_t> &payload, const start_request &request);
start_request read_start_request(const std::vector<std::uint8_t> &payload);
void append_start_response(std::vector<std::uint8_t> &payload, const start_response &response);
start_response read_start_response(const std::vector<std::uint8_t> &payload);

class protocol_decoder {
public:
    void feed(const std::uint8_t *data, std::size_t size);
    bool empty() const { return messages_.empty(); }
    bool has_buffered_input() const { return !buffer_.empty(); }
    protocol_message pop();

private:
    std::vector<std::uint8_t> buffer_;
    std::vector<protocol_message> messages_;
};

} // namespace pm_tiny

#endif
