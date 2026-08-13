#include "dependency_graph_renderer.h"

#include "dependency_graph.h"
#include "pm_tiny.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <stdexcept>

namespace pm_tiny {
namespace cli {
namespace {

using nlohmann::json;

struct graph_node_view {
    dependency_graph::node_id id = dependency_graph::npos;
    const process_list_entry *entry = nullptr;
    std::size_t layer = 0;
    std::vector<std::string> dependencies;
    std::vector<std::string> dependents;
    std::vector<std::string> blocked_by;
    std::vector<std::string> external_dependencies;
};

struct graph_view {
    std::string focus;
    std::vector<graph_node_view> nodes;
    std::size_t edge_count = 0;
};

std::string sanitize_text(const std::string &value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char ch : value) {
        if (ch < 0x20 || ch == 0x7f) result.push_back(' ');
        else result.push_back(static_cast<char>(ch));
    }
    return result;
}

std::string join(const std::vector<std::string> &values, const char *separator) {
    std::ostringstream output;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) output << separator;
        output << sanitize_text(values[i]);
    }
    return output.str();
}

std::vector<std::string> blocked_roots(const dependency_graph &graph,
                                       const std::vector<process_list_entry> &entries,
                                       dependency_graph::node_id id) {
    if (entries[id].state != PM_TINY_PROG_STATE_BLOCKED) return {};
    std::vector<std::string> result;
    std::vector<bool> visited(graph.size(), false);
    std::function<void(dependency_graph::node_id)> visit = [&](dependency_graph::node_id current) {
        for (const auto dependency : graph.dependencies(current)) {
            if (visited[dependency]) continue;
            visited[dependency] = true;
            if (entries[dependency].state == PM_TINY_PROG_STATE_STARTUP_FAIL) {
                result.push_back(graph.name(dependency));
            } else if (entries[dependency].state == PM_TINY_PROG_STATE_BLOCKED) {
                visit(dependency);
            }
        }
    };
    visit(id);
    std::sort(result.begin(), result.end(), [&](const std::string &left, const std::string &right) {
        return graph.find(left) < graph.find(right);
    });
    return result;
}

graph_view build_view(const std::vector<process_list_entry> &entries,
                      const dependency_graph_render_options &options) {
    std::vector<dependency_node_config> configs;
    configs.reserve(entries.size());
    for (const auto &entry : entries) configs.push_back({entry.name, entry.depends_on});

    dependency_graph graph;
    dependency_error error;
    if (!dependency_graph::build(configs, graph, error)) {
        throw std::runtime_error("invalid dependency graph response: " + error.message);
    }

    std::vector<bool> visible(graph.size(), options.focus.empty());
    if (!options.focus.empty()) {
        const auto focus = graph.find(options.focus);
        if (focus == dependency_graph::npos) {
            throw std::invalid_argument("process not found: " + options.focus);
        }
        std::function<void(dependency_graph::node_id)> include_dependencies =
            [&](dependency_graph::node_id id) {
                if (visible[id]) return;
                visible[id] = true;
                for (const auto dependency : graph.dependencies(id)) include_dependencies(dependency);
            };
        include_dependencies(focus);
        for (const auto dependent : graph.transitive_dependents(focus)) visible[dependent] = true;
    }

    std::vector<std::size_t> layers(graph.size(), 0);
    for (const auto id : graph.topological_order()) {
        if (!visible[id]) continue;
        for (const auto dependency : graph.dependencies(id)) {
            if (visible[dependency]) layers[id] = std::max(layers[id], layers[dependency] + 1);
        }
    }

    graph_view view;
    view.focus = options.focus;
    view.nodes.reserve(graph.size());
    for (const auto id : graph.topological_order()) {
        if (!visible[id]) continue;
        graph_node_view node;
        node.id = id;
        node.entry = &entries[id];
        node.layer = layers[id];
        for (const auto dependency : graph.dependencies(id)) {
            if (visible[dependency]) {
                node.dependencies.push_back(graph.name(dependency));
                ++view.edge_count;
            } else {
                node.external_dependencies.push_back(graph.name(dependency));
            }
        }
        for (const auto dependent : graph.dependents(id)) {
            if (visible[dependent]) node.dependents.push_back(graph.name(dependent));
        }
        node.blocked_by = blocked_roots(graph, entries, id);
        view.nodes.push_back(std::move(node));
    }
    std::stable_sort(view.nodes.begin(), view.nodes.end(), [](const graph_node_view &left,
                                                              const graph_node_view &right) {
        if (left.layer != right.layer) return left.layer < right.layer;
        return left.id < right.id;
    });
    return view;
}

const char *state_ansi(std::int32_t state) {
    switch (state) {
        case PM_TINY_PROG_STATE_RUNING: return "\033[32m";
        case PM_TINY_PROG_STATE_STARTING: return "\033[34m";
        case PM_TINY_PROG_STATE_WAITING_START: return "\033[33m";
        case PM_TINY_PROG_STATE_EXIT: return "\033[34m";
        default: return "\033[31m";
    }
}

std::string state_text(const process_list_entry &entry,
                       const dependency_graph_render_options &options) {
    const auto state = pm_state_to_str(entry.state);
    const bool color = options.stdout_is_tty && !options.no_color && std::getenv("NO_COLOR") == nullptr;
    if (!color) return state;
    return std::string(state_ansi(entry.state)) + state + "\033[0m";
}

std::string render_text(const graph_view &view,
                        const dependency_graph_render_options &options) {
    std::ostringstream output;
    output << "Dependency graph: " << view.nodes.size()
           << (view.nodes.size() == 1 ? " node, " : " nodes, ")
           << view.edge_count << (view.edge_count == 1 ? " edge" : " edges");
    if (!view.focus.empty()) output << " (focus: " << sanitize_text(view.focus) << ")";
    output << "\n";
    if (view.nodes.empty()) return output.str();

    std::size_t current_layer = static_cast<std::size_t>(-1);
    for (const auto &node : view.nodes) {
        if (node.layer != current_layer) {
            current_layer = node.layer;
            output << "\nL" << current_layer << "\n";
        }
        output << "  " << sanitize_text(node.entry->name) << " [" << state_text(*node.entry, options);
        if (!node.blocked_by.empty()) output << "; blocked_by=" << join(node.blocked_by, ",");
        output << "]";
        if (!node.dependencies.empty()) output << " <- " << join(node.dependencies, ", ");
        if (!node.external_dependencies.empty()) {
            output << (node.dependencies.empty() ? " <- " : "; ")
                   << "external: " << join(node.external_dependencies, ", ");
        }
        output << "\n";
    }
    return output.str();
}

std::string render_json(const graph_view &view) {
    json root;
    root["schema_version"] = 1;
    root["focus"] = view.focus.empty() ? json(nullptr) : json(view.focus);
    root["nodes"] = json::array();
    root["edges"] = json::array();
    for (const auto &node : view.nodes) {
        json item;
        item["name"] = node.entry->name;
        item["state"] = pm_state_to_str(node.entry->state);
        item["layer"] = node.layer;
        item["depends_on"] = node.dependencies;
        item["dependents"] = node.dependents;
        item["blocked_by"] = node.blocked_by;
        item["external_dependencies"] = node.external_dependencies;
        root["nodes"].push_back(std::move(item));
        for (const auto &dependency : node.dependencies) {
            root["edges"].push_back({{"from", dependency}, {"to", node.entry->name}});
        }
    }
    return root.dump(2) + "\n";
}

std::string dot_escape(const std::string &value) {
    std::string result;
    for (const unsigned char ch : value) {
        switch (ch) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': break;
            case '\t': result += "\\t"; break;
            default:
                if (ch < 0x20 || ch == 0x7f) result.push_back('?');
                else result.push_back(static_cast<char>(ch));
                break;
        }
    }
    return result;
}

std::string render_dot(const graph_view &view) {
    std::ostringstream output;
    output << "digraph pm_tiny {\n  rankdir=LR;\n  node [shape=box];\n";
    for (const auto &node : view.nodes) {
        std::string label = node.entry->name + "\n" + pm_state_to_str(node.entry->state);
        if (!node.blocked_by.empty()) label += "\nblocked_by: " + join(node.blocked_by, ",");
        if (!node.external_dependencies.empty()) {
            label += "\nexternal: " + join(node.external_dependencies, ",");
        }
        output << "  \"" << dot_escape(node.entry->name) << "\" [label=\""
               << dot_escape(label) << "\"];\n";
    }
    for (const auto &node : view.nodes) {
        for (const auto &dependency : node.dependencies) {
            output << "  \"" << dot_escape(dependency) << "\" -> \""
                   << dot_escape(node.entry->name) << "\";\n";
        }
    }
    output << "}\n";
    return output.str();
}

} // namespace

std::string render_dependency_graph(const std::vector<process_list_entry> &entries,
                                    const dependency_graph_render_options &options) {
    if (options.json && options.dot) throw std::invalid_argument("--json and --dot cannot be used together");
    const auto view = build_view(entries, options);
    if (options.json) return render_json(view);
    if (options.dot) return render_dot(view);
    return render_text(view, options);
}

} // namespace cli
} // namespace pm_tiny
