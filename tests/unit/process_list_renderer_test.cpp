#include "process_list_renderer.h"
#include "pm_tiny.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <sstream>

namespace {

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << std::endl;
        std::abort();
    }
}

pm_tiny::process_list_entry fixture() {
    pm_tiny::process_list_entry entry;
    entry.pid = 4321;
    entry.name = "中文服务-👩‍💻-name-with-a-long-tail";
    entry.cwd = "/very/long/path/to/服务/current";
    entry.executable = "./server";
    entry.args = {"--message=第一行\n第二行\033[31m"};
    entry.restart_count = 3;
    entry.state = PM_TINY_PROG_STATE_RUNING;
    entry.has_uptime = true;
    entry.uptime_ms = 3723000;
    entry.has_rss = true;
    entry.rss_kib = 8192;
    entry.daemon = true;
    entry.pty = pm_tiny::pty_mode_t::enabled;
    entry.depends_on = {"数据库", "cache"};
    entry.restart_pending = true;
    entry.restart_delay_remaining_ms = 2500;
    entry.restart_attempts_in_window = 2;
    entry.generation = 9;
    entry.ready = true;
    entry.heartbeat_enabled = true;
    entry.has_last_exit = true;
    entry.last_exit_reason = "exited";
    entry.last_exit_code = 4;
    entry.process_tree_backend = "job_object";
    entry.config_source = "runtime";
    entry.log_degraded = true;
    return entry;
}

} // namespace

int main() {
    const std::vector<pm_tiny::process_list_entry> entries{fixture()};

    pm_tiny::cli::list_render_options compact;
    compact.no_color = true;
    compact.terminal_width = 60;
    const auto table = pm_tiny::cli::render_process_list(entries, compact);
    expect(table.find("Total: 1") != std::string::npos, "table should include total");
    expect(table.find("online") != std::string::npos, "table should include state");
    expect(table.find('\033') == std::string::npos, "no-color table should not include ANSI escapes");
    std::istringstream lines(table);
    std::string line;
    while (std::getline(lines, line)) {
        expect(line.size() < 100, "compact output should remain bounded");
    }

    pm_tiny::cli::list_render_options wide;
    wide.wide = true;
    wide.no_color = true;
    wide.terminal_width = 160;
    const auto wide_table = pm_tiny::cli::render_process_list(entries, wide);
    expect(wide_table.find("depends_on") != std::string::npos, "wide table should include dependencies");
    auto blocked_entry = fixture();
    blocked_entry.state = PM_TINY_PROG_STATE_BLOCKED;
    pm_tiny::cli::list_render_options blocked_options;
    blocked_options.json = true;
    const auto blocked_json = pm_tiny::cli::render_process_list({blocked_entry}, blocked_options);
    expect(nlohmann::json::parse(blocked_json).at("processes").at(0).at("state") == "blocked",
           "blocked state should be rendered in JSON");
    expect(wide_table.find("retry") != std::string::npos, "wide table should include retry status");
    expect(wide_table.find("log") != std::string::npos &&
           wide_table.find("degraded") != std::string::npos,
           "wide table should include log health");
    expect(wide_table.find("👩‍💻") != std::string::npos, "truncation should retain whole grapheme clusters");
    expect(wide_table.find("第二行") != std::string::npos, "sanitized command should retain text");
    expect(wide_table.find('\n', wide_table.find("第二行")) != std::string::npos,
           "sanitized command should remain on one table row");

    pm_tiny::cli::list_render_options json_options;
    json_options.json = true;
    const auto json_text = pm_tiny::cli::render_process_list(entries, json_options);
    const auto parsed = nlohmann::json::parse(json_text);
    expect(parsed.at("schema_version") == pm_tiny::process_list_schema_version,
           "JSON schema should match wire schema");
    expect(parsed.at("total") == 1, "JSON total should match");
    expect(parsed.at("processes").at(0).at("executable") == entries[0].executable &&
           parsed.at("processes").at(0).at("args") == entries[0].args,
           "JSON should preserve structured argv");
    expect(parsed.at("processes").at(0).at("restart_pending") == true &&
           parsed.at("processes").at(0).at("restart_delay_remaining_ms") == 2500 &&
           parsed.at("processes").at(0).at("restart_attempts_in_window") == 2,
           "JSON should expose restart runtime state");
    expect(parsed.at("processes").at(0).at("generation") == 9 &&
           parsed.at("processes").at(0).at("ready") == true &&
           parsed.at("processes").at(0).at("heartbeat_enabled") == true,
           "JSON should expose generation and health state");
    expect(parsed.at("processes").at(0).at("last_exit_reason") == "exited" &&
           parsed.at("processes").at(0).at("last_exit_code") == 4 &&
           parsed.at("processes").at(0).at("process_tree_backend") == "job_object" &&
           parsed.at("processes").at(0).at("config_source") == "runtime",
           "JSON should expose common diagnostics");
    expect(json_text.find('\033') == std::string::npos,
           "JSON output should escape control bytes");

    pm_tiny::cli::list_render_options empty;
    expect(pm_tiny::cli::render_process_list({}, empty) == "Total: 0\n",
           "empty list should be concise");
    return 0;
}
