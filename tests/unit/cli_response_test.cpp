#include "cli_response.h"
#include "protocol_v3.h"

#include <cstdlib>
#include <iostream>

namespace {

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << std::endl;
        std::abort();
    }
}

pm_tiny::frame_t response(std::int32_t status, const std::string &message) {
    pm_tiny::frame_t frame;
    pm_tiny::fappend_value(frame, status);
    pm_tiny::fappend_value(frame, message);
    return frame;
}

pm_tiny::frame_t log_response(pm_tiny::log_request_mode mode) {
    auto frame = response(0, "OK");
    pm_tiny::program_log_response metadata;
    metadata.mode = mode;
    metadata.generation = 3;
    metadata.last_pid = 42;
    metadata.last_exit_time_unix_ms = mode == pm_tiny::log_request_mode::history
                                          ? 1723766400000LL : 0;
    metadata.exit_reason = mode == pm_tiny::log_request_mode::history ? "exited" : "";
    metadata.exit_code = 0;
    pm_tiny::append_program_log_response(frame, metadata);
    return frame;
}

} // namespace

int main() {
    using namespace pm_tiny;
    using namespace pm_tiny::cli;

    parsed_command stop;
    stop.kind = command_kind::stop;
    stop.name = "api";
    auto result = interpret_control_response(stop, response(0, "OK"), "4.0.0");
    expect(result.success && result.stdout_text.empty() && result.stderr_text.empty(),
           "successful stop should be silent");
    expect(result.post_list, "successful stop should request a process list by default");
    stop.no_list = true;
    result = interpret_control_response(stop, response(0, "Success"), "4.0.0");
    expect(!result.post_list, "--no-list should suppress the post-command list");

    parsed_command save;
    save.kind = command_kind::save;
    result = interpret_control_response(save, response(0, "OK"), "4.0.0");
    expect(result.success && result.stdout_text.empty() && !result.post_list,
           "successful save should be silent without a list");

    parsed_command log;
    log.kind = command_kind::log;
    result = interpret_control_response(log, log_response(log_request_mode::live), "4.0.0");
    expect(result.success && result.stream_expected && result.stdout_text.empty(),
           "log success should only enter stream mode");

    parsed_command restart;
    restart.kind = command_kind::restart;
    restart.show_log = true;
    result = interpret_control_response(restart, log_response(log_request_mode::live), "4.0.0");
    expect(result.success && result.stream_expected && !result.post_list,
           "restart --log response should enter stream mode");

    log.log_history = true;
    log.name = "app";
    result = interpret_control_response(log, log_response(log_request_mode::history), "4.0.0");
    expect(result.success && result.stream_expected &&
           result.stdout_text.find("showing cached log for stopped process `app`") != std::string::npos,
           "history response should render metadata before the stream");
    bool missing_metadata_rejected = false;
    try { (void)interpret_control_response(log, response(0, "OK"), "4.0.0"); }
    catch (const std::exception &) { missing_metadata_rejected = true; }
    expect(missing_metadata_rejected, "log response without metadata should be rejected");

    parsed_command start;
    start.kind = command_kind::start;
    start.start.name = "worker";
    start.start.create = true;
    auto started_frame = response(0, "OK");
    start_response started;
    started.result = start_result::started;
    started.pid = 42;
    append_start_response(started_frame, started);
    result = interpret_control_response(start, started_frame, "4.0.0");
    expect(result.success && result.stdout_text ==
           "started `worker` pid=42; run `pm save` to persist\n",
           "start output should use the shared format");

    auto blocked_frame = response(0, "OK");
    start_response blocked;
    blocked.result = start_result::blocked;
    blocked.blocked_by = {"database", "cache"};
    append_start_response(blocked_frame, blocked);
    result = interpret_control_response(start, blocked_frame, "4.0.0");
    expect(!result.success && result.stdout_text.empty() &&
           result.stderr_text == "pm: error(-7): blocked `worker` by: database,cache\n",
           "blocked start should be a shared stderr error");

    parsed_command inspect;
    inspect.kind = command_kind::inspect;
    result = interpret_control_response(inspect, response(-1, "ERR executable not found\r\n"), "4.0.0");
    expect(!result.success && result.stdout_text.empty() &&
           result.stderr_text == "pm: error(-1): executable not found\n",
           "daemon errors should be normalized");

    parsed_command quit;
    quit.kind = command_kind::quit;
    auto quit_frame = response(0, "OK");
    fappend_value(quit_frame, static_cast<std::int32_t>(1234));
    result = interpret_control_response(quit, quit_frame, "4.0.0");
    expect(result.success && result.daemon_pid == 1234 && result.stdout_text.empty(),
           "quit should remain silent and expose the daemon pid");
    return 0;
}
