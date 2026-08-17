#pragma once

#include "cli_command.h"
#include "frame_stream.hpp"

#include <cstdint>
#include <string>

namespace pm_tiny {
namespace cli {

struct cli_response_result {
    bool success = false;
    bool stream_expected = false;
    bool post_list = false;
    std::int64_t daemon_pid = -1;
    program_log_response log_response;
    std::string stdout_text;
    std::string stderr_text;
};

cli_response_result interpret_control_response(
    const parsed_command &command,
    const frame_t &payload,
    const std::string &client_version,
    const list_render_options *list_options = nullptr,
    const dependency_graph_render_options *graph_options = nullptr);

} // namespace cli
} // namespace pm_tiny
