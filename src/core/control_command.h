#ifndef PM_TINY_CONTROL_COMMAND_H
#define PM_TINY_CONTROL_COMMAND_H

#include "frame_stream.hpp"
#include "protocol_v3.h"

#include <cstdint>
#include <string>

namespace pm_tiny {

enum class control_command : std::uint16_t {
    list = 0x23,
    stop = 0x24,
    start = 0x25,
    save = 0x26,
    remove = 0x27,
    restart = 0x28,
    version = 0x29,
    log = 0x30,
    app_ready = 0x31,
    app_tick = 0x32,
    inspect = 0x33,
    reload = 0x34,
    quit = 0x35,
    info = 0x36,
};

struct decoded_control_request {
    bool success = false;
    control_command command = control_command::list;
    std::string name;
    std::string error;
};

bool is_known_control_command(std::uint16_t type);
bool control_command_requires_name(control_command command);
const char *control_command_name(control_command command);
decoded_control_request decode_control_request(const protocol_message &request);

frame_ptr_t make_control_response_payload(std::int32_t status, const std::string &message);
protocol_message make_control_response(const protocol_message &request,
                                       std::int32_t status,
                                       const std::string &message);

} // namespace pm_tiny

#endif
