#include "protocol_v2.h"

#include <algorithm>
#include <cstring>

namespace pm_tiny {
namespace {
constexpr std::size_t header_size = 16;
constexpr char magic[] = {'P', 'M', 'T', '2'};

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
}

std::vector<std::uint8_t> protocol_encode(const protocol_message &message) {
    if (message.payload.size() > protocol_v2_max_payload) {
        throw protocol_error("protocol payload exceeds maximum size");
    }
    validate_flags(message.flags);
    std::vector<std::uint8_t> out(header_size + message.payload.size());
    std::memcpy(out.data(), magic, sizeof(magic));
    out[4] = protocol_v2_version;
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
        if (!std::equal(buffer_.begin(), buffer_.begin() + 4, magic)) {
            throw protocol_error("invalid protocol magic");
        }
        if (buffer_[4] != protocol_v2_version) {
            throw protocol_error("unsupported protocol version");
        }
        validate_flags(buffer_[5]);
        const auto payload_size = get32(buffer_.data() + 12);
        if (payload_size > protocol_v2_max_payload) {
            throw protocol_error("protocol payload exceeds maximum size");
        }
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
}
