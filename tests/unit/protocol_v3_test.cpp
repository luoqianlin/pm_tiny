#include "core/protocol_v3.h"
#include "core/frame_stream.hpp"

#include <cassert>
#include <cstdint>

int main() {
    pm_tiny::protocol_message message;
    message.type = 0x25;
    message.flags = pm_tiny::protocol_flag_response;
    message.request_id = 0x10203040;
    message.payload = {0, 1, 2, 0xff};
    auto encoded = pm_tiny::protocol_encode(message);
    assert(encoded.size() == 20);
    assert(encoded[0] == 'P' && encoded[1] == 'M' && encoded[2] == 'T' && encoded[3] == '3');
    assert(encoded[6] == 0 && encoded[7] == 0x25);
    assert(encoded[8] == 0x10 && encoded[9] == 0x20 && encoded[10] == 0x30 && encoded[11] == 0x40);

    pm_tiny::protocol_decoder decoder;
    for (auto byte : encoded) decoder.feed(&byte, 1);
    assert(!decoder.empty());
    auto decoded = decoder.pop();
    assert(decoded.type == message.type);
    assert(decoded.flags == message.flags);
    assert(decoded.request_id == message.request_id);
    assert(decoded.payload == message.payload);

    pm_tiny::protocol_message second_message;
    second_message.type = 0x29;
    second_message.request_id = 0x50607080;
    auto second_encoded = pm_tiny::protocol_encode(second_message);
    std::vector<std::uint8_t> coalesced = encoded;
    coalesced.insert(coalesced.end(), second_encoded.begin(), second_encoded.end());
    pm_tiny::protocol_decoder coalesced_decoder;
    coalesced_decoder.feed(coalesced.data(), coalesced.size());
    assert(coalesced_decoder.pop().request_id == message.request_id);
    assert(coalesced_decoder.pop().request_id == second_message.request_id);
    assert(coalesced_decoder.empty());

    auto malformed = encoded;
    malformed[0] = 'X';
    bool rejected = false;
    try { pm_tiny::protocol_decoder().feed(malformed.data(), malformed.size()); }
    catch (const pm_tiny::protocol_error &) { rejected = true; }
    assert(rejected);

    auto invalid_flags = message;
    invalid_flags.flags = 0x80;
    rejected = false;
    try { pm_tiny::protocol_encode(invalid_flags); }
    catch (const pm_tiny::protocol_error &) { rejected = true; }
    assert(rejected);

    invalid_flags.flags = pm_tiny::protocol_flag_more;
    rejected = false;
    try { pm_tiny::protocol_encode(invalid_flags); }
    catch (const pm_tiny::protocol_error &) { rejected = true; }
    assert(rejected);

    pm_tiny::start_request start;
    start.mode = pm_tiny::start_mode::create;
    start.name = "api";
    start.config.name = "api";
    start.config.cwd = "/opt/api";
    start.config.executable = "/opt/api/server";
    start.config.args = {"--listen", "0.0.0.0:8080", "", "-x"};
    start.config.env_vars = {"PORT=8080"};
    start.config.depends_on = {"database"};
    start.config.log_mode = pm_tiny::log_mode_t::combined;
    start.config.log_dir = "logs";
    start.config.log_file_name = "api.log";
    start.config.log_max_size_kb = 8192;
    start.config.log_archive_count = 7;
    start.inherited_env = {"PATH=/usr/bin", "LANG=C"};
    start.show_log = true;
    std::vector<std::uint8_t> start_payload;
    pm_tiny::append_start_request(start_payload, start);
    const auto decoded_start = pm_tiny::read_start_request(start_payload);
    assert(decoded_start.mode == start.mode && decoded_start.name == start.name);
    assert(decoded_start.config.executable == start.config.executable);
    assert(decoded_start.config.args == start.config.args);
    assert(decoded_start.config.env_vars == start.config.env_vars);
    assert(decoded_start.config.log_mode == start.config.log_mode &&
           decoded_start.config.log_dir == start.config.log_dir &&
           decoded_start.config.log_file_name == start.config.log_file_name &&
           decoded_start.config.log_max_size_kb == 8192 && decoded_start.config.log_archive_count == 7);
    pm_tiny::frame_t old_config;
    pm_tiny::fappend_value<std::int32_t>(old_config, 1);
    rejected = false;
    try {
        pm_tiny::iframe_stream old_config_stream(old_config);
        (void)pm_tiny::read_prog_cfg(old_config_stream);
    } catch (const pm_tiny::protocol_error &) { rejected = true; }
    assert(rejected);
    assert(decoded_start.inherited_env == start.inherited_env && decoded_start.show_log);

    pm_tiny::start_response start_response;
    start_response.result = pm_tiny::start_result::waiting;
    start_response.pid = -1;
    start_response.state = "waiting";
    start_response.blocked_by = {"database"};
    start_response.message = "dependency is starting";
    std::vector<std::uint8_t> response_payload;
    pm_tiny::append_start_response(response_payload, start_response);
    const auto decoded_response = pm_tiny::read_start_response(response_payload);
    assert(decoded_response.result == start_response.result && decoded_response.pid == -1);
    assert(decoded_response.state == "waiting" && decoded_response.blocked_by == start_response.blocked_by);

    auto v2 = encoded;
    v2[3] = '2';
    rejected = false;
    try { pm_tiny::protocol_decoder().feed(v2.data(), v2.size()); }
    catch (const pm_tiny::protocol_error &) { rejected = true; }
    assert(rejected);
    return 0;
}
