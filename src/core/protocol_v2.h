#ifndef PM_TINY_PROTOCOL_V2_H
#define PM_TINY_PROTOCOL_V2_H

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace pm_tiny {

constexpr std::uint32_t protocol_v2_max_payload = 4u * 1024u * 1024u;
constexpr std::uint8_t protocol_v2_version = 2;

enum protocol_v2_flags : std::uint8_t {
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
};

std::vector<std::uint8_t> protocol_encode(const protocol_message &message);

class protocol_decoder {
public:
    void feed(const std::uint8_t *data, std::size_t size);
    bool empty() const { return messages_.empty(); }
    protocol_message pop();

private:
    std::vector<std::uint8_t> buffer_;
    std::vector<protocol_message> messages_;
};

}

#endif
