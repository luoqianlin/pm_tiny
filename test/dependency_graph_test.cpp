#include "dependency_graph.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "dependency_graph_test: " << message << std::endl;
        std::abort();
    }
}

pm_tiny::dependency_node_config node(const std::string &name,
                                     std::vector<std::string> dependencies = {}) {
    return {name, std::move(dependencies)};
}

std::vector<std::string> names(const pm_tiny::dependency_graph &graph,
                               const std::vector<pm_tiny::dependency_graph::node_id> &ids) {
    std::vector<std::string> result;
    for (const auto id : ids) result.push_back(graph.name(id));
    return result;
}

} // namespace

int main() {
    using namespace pm_tiny;
    dependency_graph graph;
    dependency_error error;
    expect(dependency_graph::build({}, graph, error) && graph.empty(), "empty graph should be valid");

    const std::vector<dependency_node_config> diamond = {
        node("right", {"root"}), node("root"), node("left", {"root"}), node("leaf", {"left", "right"})
    };
    expect(dependency_graph::build(diamond, graph, error), "diamond should be valid");
    expect(names(graph, graph.topological_order()) == std::vector<std::string>({"root", "right", "left", "leaf"}),
           "topological order should be stable");
    expect(names(graph, graph.reverse_topological_order()) == std::vector<std::string>({"leaf", "left", "right", "root"}),
           "reverse order mismatch");
    expect(names(graph, graph.transitive_dependents(graph.find("root"))) ==
           std::vector<std::string>({"right", "left", "leaf"}), "transitive dependents mismatch");

    dependency_runtime runtime(graph);
    expect(runtime.request_all() == std::vector<std::string>({"root"}), "only root should start initially");
    expect(runtime.mark_ready("root") == std::vector<std::string>({"right", "left"}), "both branches should unlock");
    expect(runtime.mark_ready("right").empty(), "leaf must wait for both dependencies");
    expect(runtime.mark_ready("left") == std::vector<std::string>({"leaf"}), "leaf should unlock once");
    expect(runtime.mark_ready("left").empty(), "ready must be idempotent");

    runtime.reset(graph);
    runtime.request_all();
    runtime.mark_ready("root");
    const auto failure = runtime.mark_failed("right");
    expect(failure.blocked == std::vector<std::string>({"leaf"}), "failure should block only downstream");
    expect(runtime.state("left") == dependency_runtime_state::starting, "sibling branch should continue");
    runtime.mark_ready("left");
    expect(runtime.mark_ready("right") == std::vector<std::string>({"leaf"}), "recovery should unlock downstream");

    dependency_graph multi_graph;
    expect(dependency_graph::build({node("a"), node("b"), node("leaf", {"a", "b"})},
                                   multi_graph, error), "multi dependency graph should be valid");
    runtime.reset(multi_graph);
    runtime.request_all();
    runtime.mark_failed("a");
    runtime.mark_failed("b");
    expect(runtime.mark_ready("a").empty() && runtime.state("leaf") == dependency_runtime_state::blocked,
           "one recovered dependency must not clear another blocker");
    expect(runtime.blocked_by("leaf") == std::vector<std::string>({"b"}), "remaining blocker mismatch");
    expect(runtime.mark_ready("b") == std::vector<std::string>({"leaf"}), "all blockers recovered should unlock");

    dependency_graph invalid;
    expect(!dependency_graph::build({node("a", {"missing"})}, invalid, error) &&
           error.code == dependency_error_code::missing_dependency, "missing dependency should fail");
    expect(!dependency_graph::build({node("a", {"a"})}, invalid, error) &&
           error.code == dependency_error_code::self_dependency, "self dependency should fail");
    expect(!dependency_graph::build({node("a", {"b", "b"}), node("b")}, invalid, error) &&
           error.code == dependency_error_code::duplicate_dependency, "duplicate dependency should fail");
    expect(!dependency_graph::build({node("a", {"b"}), node("b", {"c"}), node("c", {"a"})}, invalid, error) &&
           error.cycle_path == std::vector<std::string>({"a", "b", "c", "a"}), "cycle path mismatch");
    return 0;
}
