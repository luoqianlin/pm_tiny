#include "fuzz_adapters.h"

#include "dependency_graph.h"
#include "frame_stream.hpp"
#include "prog_cfg_order.h"
#include "prog_cfg_yaml_helper.h"
#include "program_log.h"
#include "protocol_v3.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

namespace pm_tiny {
namespace fuzz {
namespace {

void require(bool condition) {
    if (!condition) std::abort();
}

void round_trip(const protocol_message &message) {
    const auto encoded = protocol_encode(message);
    protocol_decoder decoder;
    decoder.feed(encoded.data(), encoded.size());
    require(!decoder.empty());
    const auto decoded = decoder.pop();
    require(decoded.type == message.type);
    require(decoded.flags == message.flags);
    require(decoded.request_id == message.request_id);
    require(decoded.payload == message.payload);
    require(decoder.empty());
}

} // namespace

void protocol_input(const std::uint8_t *data, std::size_t size) {
    if (data == nullptr || size == 0) return;
    try {
        protocol_decoder decoder;
        const std::size_t stride = static_cast<std::size_t>(data[0] % 31U) + 1U;
        for (std::size_t offset = 0; offset < size;) {
            const std::size_t chunk = std::min(stride, size - offset);
            decoder.feed(data + offset, chunk);
            offset += chunk;
        }
        while (!decoder.empty()) round_trip(decoder.pop());
    } catch (const std::exception &) {
    }
    try {
        read_start_request(std::vector<std::uint8_t>(data, data + size));
    } catch (const std::exception &) {
    }
}

void config_input(const std::uint8_t *data, std::size_t size) {
    if (data == nullptr || size == 0 || size > protocol_v3_max_payload) return;
    try {
        const YAML::Node root = YAML::Load(std::string(reinterpret_cast<const char *>(data), size));
        auto parsed = parse_prog_cfg_yaml_document(root);
        if (!parsed.success) return;
        std::string error;
        validate_and_order_prog_cfgs(parsed.programs, error);
    } catch (const std::exception &) {
    }
}

void log_input(const std::uint8_t *data, std::size_t size) {
    if (data == nullptr || size == 0) return;
    const std::vector<std::uint8_t> input(data, data + size);
    try {
        read_program_log_request(input);
    } catch (const std::exception &) {
    }
    try {
        iframe_stream stream(input);
        read_program_log_response(stream);
    } catch (const std::exception &) {
    }
    bounded_log_tail tail(data[0] % 128U);
    const std::size_t midpoint = size / 2U;
    tail.append(reinterpret_cast<const char *>(data), midpoint);
    tail.append(reinterpret_cast<const char *>(data + midpoint), size - midpoint);
    const auto snapshot = tail.snapshot();
    require(snapshot.size() <= tail.size());
    if (!snapshot.empty()) require(snapshot == tail.read(tail.begin_offset(), snapshot.size()));
}

} // namespace fuzz
} // namespace pm_tiny
