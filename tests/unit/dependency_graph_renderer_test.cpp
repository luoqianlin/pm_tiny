#include "dependency_graph_renderer.h"
#include "pm_tiny.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << std::endl;
        std::abort();
    }
}

pm_tiny::process_list_entry node(std::string name, std::int32_t state,
                                 std::vector<std::string> dependencies = {}) {
    pm_tiny::process_list_entry entry;
    entry.name = std::move(name);
    entry.state = state;
    entry.depends_on = std::move(dependencies);
    return entry;
}

std::vector<pm_tiny::process_list_entry> fixture() {
    return {
        node("db", PM_TINY_PROG_STATE_RUNING),
        node("cache", PM_TINY_PROG_STATE_STARTUP_FAIL),
        node("api", PM_TINY_PROG_STATE_BLOCKED, {"db", "cache"}),
        node("worker", PM_TINY_PROG_STATE_BLOCKED, {"api"}),
        node("side", PM_TINY_PROG_STATE_RUNING, {"db"}),
    };
}

} // namespace

int main() {
    using pm_tiny::cli::dependency_graph_render_options;
    using pm_tiny::cli::render_dependency_graph;

    const auto entries = fixture();
    dependency_graph_render_options text_options;
    text_options.no_color = true;
    const auto text = render_dependency_graph(entries, text_options);
    expect(text.find("Dependency graph: 5 nodes, 4 edges") != std::string::npos,
           "text summary should include node and edge counts");
    expect(text.find("api [blocked; blocked_by=cache] <- db, cache") != std::string::npos,
           "text should show direct blocked root");
    expect(text.find("worker [blocked; blocked_by=cache] <- api") != std::string::npos,
           "text should trace blocked root through blocked dependencies");
    expect(text.find("\033[") == std::string::npos, "--no-color should suppress ANSI output");

    dependency_graph_render_options color_options;
    color_options.stdout_is_tty = true;
    const auto colored = render_dependency_graph(entries, color_options);
    if (std::getenv("NO_COLOR") == nullptr) {
        expect(colored.find("\033[31mblocked\033[0m") != std::string::npos,
               "terminal output should color states");
    } else {
        expect(colored.find("\033[") == std::string::npos,
               "NO_COLOR should suppress ANSI output");
    }

    dependency_graph_render_options json_options;
    json_options.json = true;
    const auto parsed = nlohmann::json::parse(render_dependency_graph(entries, json_options));
    expect(parsed.at("schema_version") == 1 && parsed.at("focus").is_null(),
           "JSON schema metadata should be stable");
    expect(parsed.at("nodes").size() == 5 && parsed.at("edges").size() == 4,
           "JSON should contain all nodes and edges");
    bool saw_worker = false;
    for (const auto &item : parsed.at("nodes")) {
        if (item.at("name") == "worker") {
            saw_worker = item.at("layer") == 2 &&
                         item.at("blocked_by") == nlohmann::json::array({"cache"});
        }
    }
    expect(saw_worker, "JSON should preserve stable layers and blocked roots");

    dependency_graph_render_options focus_options;
    focus_options.focus = "db";
    focus_options.json = true;
    const auto focused = nlohmann::json::parse(render_dependency_graph(entries, focus_options));
    expect(focused.at("nodes").size() == 4, "focused graph should include ancestors and descendants only");
    bool saw_external_cache = false;
    for (const auto &item : focused.at("nodes")) {
        if (item.at("name") == "api") {
            saw_external_cache = item.at("external_dependencies") == nlohmann::json::array({"cache"});
        }
    }
    expect(saw_external_cache, "focused graph should report dependencies outside the view");

    dependency_graph_render_options dot_options;
    dot_options.dot = true;
    const auto dot = render_dependency_graph(entries, dot_options);
    expect(dot.find("rankdir=LR") != std::string::npos &&
           dot.find("\"cache\" -> \"api\"") != std::string::npos,
           "DOT should use dependency-to-dependent edge direction");
    expect(dot.find("api\\nblocked\\nblocked_by: cache") != std::string::npos,
           "DOT labels should contain Graphviz newlines and blocked roots");

    expect(render_dependency_graph({}, text_options) == "Dependency graph: 0 nodes, 0 edges\n",
           "empty graph should be concise");
    expect(render_dependency_graph({node("only", PM_TINY_PROG_STATE_RUNING)}, text_options).find(
               "Dependency graph: 1 node, 0 edges") == 0,
           "singular node summary should be grammatical");
    bool missing_failed = false;
    try {
        dependency_graph_render_options missing;
        missing.focus = "missing";
        (void) render_dependency_graph(entries, missing);
    } catch (const std::invalid_argument &) {
        missing_failed = true;
    }
    expect(missing_failed, "missing focus should fail");
    return 0;
}
