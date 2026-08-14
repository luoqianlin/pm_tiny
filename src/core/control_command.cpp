#include "control_command.h"

namespace pm_tiny {

bool is_known_control_command(std::uint16_t type) {
    switch (static_cast<control_command>(type)) {
        case control_command::list:
        case control_command::stop:
        case control_command::start:
        case control_command::save:
        case control_command::remove:
        case control_command::restart:
        case control_command::version:
        case control_command::log:
        case control_command::app_ready:
        case control_command::app_tick:
        case control_command::inspect:
        case control_command::reload:
        case control_command::quit:
        case control_command::info:
            return true;
    }
    return false;
}

bool control_command_requires_name(control_command command) {
    switch (command) {
        case control_command::stop:
        case control_command::remove:
        case control_command::restart:
        case control_command::log:
        case control_command::app_ready:
        case control_command::app_tick:
        case control_command::inspect:
            return true;
        case control_command::list:
        case control_command::start:
        case control_command::save:
        case control_command::version:
        case control_command::reload:
        case control_command::quit:
        case control_command::info:
            return false;
    }
    return false;
}

const char *control_command_name(control_command command) {
    switch (command) {
        case control_command::list: return "list";
        case control_command::stop: return "stop";
        case control_command::start: return "start";
        case control_command::save: return "save";
        case control_command::remove: return "delete";
        case control_command::restart: return "restart";
        case control_command::version: return "version";
        case control_command::log: return "log";
        case control_command::app_ready: return "ready";
        case control_command::app_tick: return "tick";
        case control_command::inspect: return "inspect";
        case control_command::reload: return "reload";
        case control_command::quit: return "quit";
        case control_command::info: return "info";
    }
    return "unknown";
}

decoded_control_request decode_control_request(const protocol_message &request) {
    decoded_control_request result;
    if (!is_known_control_command(request.type)) {
        result.error = "unknown message type";
        return result;
    }
    result.command = static_cast<control_command>(request.type);
    if (result.command == control_command::info && !request.payload.empty()) {
        result.error = "info request payload must be empty";
        return result;
    }
    if (control_command_requires_name(result.command)) {
        try {
            iframe_stream stream(request.payload);
            stream >> result.name;
        } catch (const std::exception &) {
            result.error = "missing process name";
            return result;
        }
        if (result.name.empty()) {
            result.error = "missing process name";
            return result;
        }
    }
    result.success = true;
    return result;
}

frame_ptr_t make_control_response_payload(std::int32_t status, const std::string &message) {
    auto payload = std::make_unique<frame_t>();
    fappend_value(*payload, status);
    fappend_value(*payload, message);
    return payload;
}

protocol_message make_control_response(const protocol_message &request,
                                       std::int32_t status,
                                       const std::string &message) {
    protocol_message response;
    response.type = request.type;
    response.request_id = request.request_id;
    response.flags = static_cast<std::uint8_t>(protocol_flag_response |
        (status == 0 ? 0 : protocol_flag_error));
    auto payload = make_control_response_payload(status, message);
    response.payload = std::move(*payload);
    return response;
}

} // namespace pm_tiny
