#include "inspect_renderer.h"
#include "pm_tiny.h"
#include "runtime_snapshot.h"

#include <cstdlib>
#include <iostream>

namespace {

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << std::endl;
        std::abort();
    }
}

} // namespace

int main() {
    pm_tiny::inspect_snapshot input;
    input.config.name = "api";
    input.config.cwd = "/srv/api";
    input.config.executable = "/srv/api/server";
    input.config.args = {"--listen", "0.0.0.0:8080"};
    input.config.depends_on = {"database"};
    input.config.daemon = true;
    input.config.heartbeat_timeout = 5;
    input.runtime.pid = 321;
    input.runtime.generation = 7;
    input.runtime.state = PM_TINY_PROG_STATE_RUNING;
    input.runtime.ready = true;
    input.runtime.heartbeat_enabled = true;
    input.runtime.has_last_tick_age = true;
    input.runtime.last_tick_age_ms = 250;
    input.runtime.has_uptime = true;
    input.runtime.uptime_ms = 4000;
    input.runtime.has_rss = true;
    input.runtime.rss_kib = 8192;
    input.runtime.pty = pm_tiny::pty_mode_t::unsupported;
    input.runtime.has_last_exit = true;
    input.runtime.last_exit_reason = pm_tiny::exit_reason_t::exited;
    input.runtime.last_exit_code = 3;
    input.runtime.process_tree_backend = "job_object";
    input.runtime.config_source = "runtime";
    input.runtime.log_degraded = true;
    input.runtime.log_dropped_bytes = 55;
    input.runtime.log_last_error = "permission denied";
    input.runtime.log_retry_remaining_ms = 1000;
    input.runtime.log_paths = {"logs/api.log"};

    pm_tiny::frame_t frame;
    pm_tiny::append_inspect_snapshot(frame, input);
    pm_tiny::iframe_stream stream(frame);
    const auto actual = pm_tiny::read_inspect_snapshot(stream);
    expect(actual.config.name == input.config.name && actual.config.args == input.config.args,
           "inspect config should round-trip");
    expect(actual.runtime.generation == 7 && actual.runtime.ready &&
           actual.runtime.last_exit_code == 3 && actual.runtime.process_tree_backend == "job_object",
           "inspect runtime should round-trip");
    expect(actual.runtime.log_degraded && actual.runtime.log_dropped_bytes == 55 &&
           actual.runtime.log_paths == input.runtime.log_paths,
           "inspect log health should round-trip");
    expect(stream.remaining_size() == 0, "inspect decoder should consume all fields");

    const auto rendered = pm_tiny::cli::render_inspect_snapshot(actual);
    expect(rendered.find("generation") != std::string::npos &&
           rendered.find("job_object") != std::string::npos &&
           rendered.find("config_source") != std::string::npos &&
           rendered.find("unsupported") != std::string::npos,
           "inspect renderer should expose common diagnostics");
    expect(rendered.find("permission denied") != std::string::npos &&
           rendered.find("logs/api.log") != std::string::npos,
           "inspect renderer should expose log diagnostics");
    return 0;
}
