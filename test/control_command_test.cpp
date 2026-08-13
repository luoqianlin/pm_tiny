#include "control_command.h"

#include <cstdlib>
#include <iostream>

namespace {
void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << std::endl;
        std::abort();
    }
}
}

int main() {
    pm_tiny::protocol_message stop;
    stop.type = static_cast<std::uint16_t>(pm_tiny::control_command::stop);
    stop.request_id = 42;
    pm_tiny::fappend_value(stop.payload, std::string("app"));
    const auto decoded = pm_tiny::decode_control_request(stop);
    expect(decoded.success && decoded.name == "app", "named request should decode");

    pm_tiny::protocol_message missing;
    missing.type = stop.type;
    expect(!pm_tiny::decode_control_request(missing).success, "missing name should fail");

    pm_tiny::protocol_message list;
    list.type = static_cast<std::uint16_t>(pm_tiny::control_command::list);
    expect(pm_tiny::decode_control_request(list).success, "list should not require a name");

    pm_tiny::protocol_message unknown;
    unknown.type = 0x99;
    expect(!pm_tiny::decode_control_request(unknown).success, "unknown command should fail");

    pm_tiny::protocol_message reserved;
    reserved.type = 0x2a;
    expect(!pm_tiny::decode_control_request(reserved).success, "reserved command gap should fail");

    const auto response = pm_tiny::make_control_response(stop, -1, "not found");
    expect(response.type == stop.type && response.request_id == 42, "response identity should match request");
    expect((response.flags & pm_tiny::protocol_flag_error) != 0, "failed response should set error flag");
    pm_tiny::iframe_stream payload(response.payload);
    std::int32_t status = 0;
    std::string text;
    payload >> status >> text;
    expect(status == -1 && text == "not found", "response payload should round-trip");
    return 0;
}
