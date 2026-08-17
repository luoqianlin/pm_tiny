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

void test_validation_and_atomic_build() {
    using namespace pm_tiny;
    dependency_graph graph;
    dependency_error error;
    expect(dependency_graph::build({node("preserved")}, graph, error), "baseline graph should build");

    struct invalid_case {
        std::vector<dependency_node_config> nodes;
        dependency_error_code code;
        const char *message;
        std::vector<std::string> cycle;
    };
    const std::vector<invalid_case> cases = {
        {{node("")}, dependency_error_code::empty_name, "Program name cannot be empty", {}},
        {{node("dup"), node("dup")}, dependency_error_code::duplicate_name,
         "Duplicate program name: dup", {}},
        {{node("app", {"dep", "dep"}), node("dep")}, dependency_error_code::duplicate_dependency,
         "Program `app` has duplicate dependency `dep`", {}},
        {{node("self", {"self"})}, dependency_error_code::self_dependency,
         "Program `self` cannot depend on itself", {}},
        {{node("app", {"missing"})}, dependency_error_code::missing_dependency,
         "Program `app` depends on missing `missing`", {}},
        {{node("a", {"b"}), node("b", {"c"}), node("c", {"a"})}, dependency_error_code::cycle,
         "Program dependency cycle detected: a -> b -> c -> a", {"a", "b", "c", "a"}},
    };
    for (const auto &item : cases) {
        expect(!dependency_graph::build(item.nodes, graph, error), "invalid graph should fail");
        expect(error.code == item.code, "validation error code mismatch");
        expect(error.message == item.message, "validation error message mismatch");
        expect(error.cycle_path == item.cycle, "validation cycle path mismatch");
        expect(graph.size() == 1 && graph.name(0) == "preserved",
               "failed build must not replace the previous graph");
    }

    expect(dependency_graph::build({}, graph, error) && graph.empty(), "empty graph should be valid");
    expect(error.code == dependency_error_code::none && error.message.empty() && error.cycle_path.empty(),
           "successful build should clear the previous error");
}

void test_stable_orders_and_queries() {
    using namespace pm_tiny;
    dependency_graph graph;
    dependency_error error;
    const std::vector<dependency_node_config> complex = {
        node("unrelated"), node("right", {"root"}), node("root"), node("left", {"root"}),
        node("merge", {"left", "right"}), node("tail", {"merge"}), node("second", {"left", "unrelated"})
    };
    expect(dependency_graph::build(complex, graph, error), "complex graph should build");
    expect(graph.find("missing") == dependency_graph::npos, "missing lookup should return npos");
    expect(names(graph, graph.topological_order()) ==
           std::vector<std::string>({"unrelated", "root", "right", "left", "merge", "tail", "second"}),
           "stable topological order mismatch");
    expect(names(graph, graph.reverse_topological_order()) ==
           std::vector<std::string>({"second", "tail", "merge", "left", "right", "root", "unrelated"}),
           "stable reverse topological order mismatch");
    expect(names(graph, graph.transitive_dependents(graph.find("root"))) ==
           std::vector<std::string>({"right", "left", "merge", "tail", "second"}),
           "transitive dependent order mismatch");
    expect(graph.dependencies(graph.find("unrelated")).empty(), "independent node should have no dependency");
}

void test_runtime_transitions_and_closure() {
    using namespace pm_tiny;
    dependency_graph graph;
    dependency_error error;
    expect(dependency_graph::build({node("side"), node("root"), node("middle", {"root"}),
                                    node("leaf", {"middle"})}, graph, error), "chain graph should build");
    dependency_runtime runtime(graph);
    expect(runtime.all_terminal(), "idle runtime should be terminal");
    expect(runtime.request_closure("missing").empty(), "missing closure should be empty");
    expect(runtime.request_closure("leaf") == std::vector<std::string>({"root"}),
           "target closure should start only its root");
    expect(runtime.state("side") == dependency_runtime_state::idle,
           "target closure must not request unrelated nodes");
    expect(!runtime.all_terminal(), "starting closure should not be terminal");
    expect(runtime.request_closure("leaf").empty(), "duplicate request should be idempotent");
    expect(runtime.mark_ready("root") == std::vector<std::string>({"middle"}), "middle should unlock");
    runtime.mark_starting("middle");
    expect(runtime.state("middle") == dependency_runtime_state::starting, "mark_starting mismatch");
    expect(runtime.mark_ready("middle") == std::vector<std::string>({"leaf"}), "leaf should unlock");
    expect(runtime.mark_ready("leaf").empty() && runtime.all_terminal(), "ready closure should be terminal");

    runtime.mark_pending("leaf");
    expect(runtime.state("leaf") == dependency_runtime_state::pending && !runtime.all_terminal(),
           "pending should be non-terminal");
    runtime.mark_idle("leaf");
    expect(runtime.state("leaf") == dependency_runtime_state::idle && runtime.all_terminal(),
           "idle should clear requested state");
    runtime.mark_starting("missing");
    runtime.mark_pending("missing");
    runtime.mark_idle("missing");
    expect(runtime.state("missing") == dependency_runtime_state::idle,
           "unknown runtime node should remain idle");
    expect(runtime.blocked_by("missing").empty() && runtime.waiting_for("missing").empty(),
           "unknown runtime queries should be empty");

    runtime.reset(graph);
    expect(runtime.request_all() == std::vector<std::string>({"side", "root"}),
           "request_all should start every independent root");
    expect(runtime.request_all().empty(), "repeated request_all should be idempotent");
}

void test_failure_propagation_and_recovery() {
    using namespace pm_tiny;
    dependency_graph graph;
    dependency_error error;
    expect(dependency_graph::build({node("a"), node("b"), node("left", {"a"}), node("right", {"b"}),
                                    node("merge", {"left", "right"}), node("tail", {"merge"})},
                                   graph, error), "failure graph should build");
    dependency_runtime runtime(graph);
    runtime.request_all();
    auto failure = runtime.mark_failed("a");
    expect(failure.failed == std::vector<std::string>({"a"}) &&
           failure.blocked == std::vector<std::string>({"left", "merge", "tail"}),
           "first failure should recursively block requested descendants");
    runtime.mark_failed("b");
    expect(runtime.blocked_by("tail") == std::vector<std::string>({"a", "b"}),
           "recursive blocker roots should be stable and complete");
    expect(runtime.waiting_for("tail") == std::vector<std::string>({"a", "b", "left", "right", "merge"}),
           "recursive waiting set should be stable and complete");
    expect(runtime.mark_ready("a") == std::vector<std::string>({"left"}),
           "recovering one root should only unlock its branch");
    runtime.mark_ready("left");
    expect(runtime.state("merge") == dependency_runtime_state::blocked,
           "merge must retain the other failure root");
    expect(runtime.mark_ready("b") == std::vector<std::string>({"right"}), "right should recover");
    expect(runtime.mark_ready("right") == std::vector<std::string>({"merge"}), "merge should recover once");
    expect(runtime.mark_ready("merge") == std::vector<std::string>({"tail"}), "tail should recover");

    runtime.reset(graph);
    runtime.request_all();
    runtime.mark_ready("a");
    runtime.mark_ready("left");
    runtime.mark_ready("b");
    runtime.mark_ready("right");
    runtime.mark_ready("merge");
    runtime.mark_ready("tail");
    failure = runtime.mark_failed("a");
    expect(failure.blocked.empty() && runtime.state("tail") == dependency_runtime_state::ready,
           "online descendants must not be blocked when a dependency later exits");
    expect(runtime.mark_failed("missing").failed.empty(), "unknown failure should be ignored");
}

void test_snapshot_migration_by_name() {
    using namespace pm_tiny;
    dependency_graph original;
    dependency_graph changed;
    dependency_error error;
    expect(dependency_graph::build({node("remove"), node("alpha"), node("beta", {"alpha"})},
                                   original, error), "original graph should build");
    dependency_runtime runtime(original);
    runtime.mark_ready("remove");
    runtime.mark_starting("alpha");
    runtime.mark_failed("beta");
    const auto snapshot = runtime.snapshot();

    expect(dependency_graph::build({node("beta"), node("added"), node("alpha", {"added"})},
                                   changed, error), "changed graph should build");
    runtime.migrate(changed, snapshot);
    expect(runtime.state("beta") == dependency_runtime_state::failed,
           "reordered node state should migrate by name");
    expect(runtime.state("alpha") == dependency_runtime_state::starting,
           "dependency changes must not move state by old index");
    expect(runtime.state("added") == dependency_runtime_state::idle,
           "new node should start idle");
    expect(runtime.state("remove") == dependency_runtime_state::idle,
           "deleted node should disappear");

    dependency_runtime empty_runtime;
    expect(empty_runtime.snapshot().empty() && empty_runtime.state("anything") == dependency_runtime_state::idle,
           "default runtime should be safe to inspect");
}

} // namespace

int main() {
    test_validation_and_atomic_build();
    test_stable_orders_and_queries();
    test_runtime_transitions_and_closure();
    test_failure_propagation_and_recovery();
    test_snapshot_migration_by_name();
    return 0;
}
