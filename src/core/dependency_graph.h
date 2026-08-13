#ifndef PM_TINY_DEPENDENCY_GRAPH_H
#define PM_TINY_DEPENDENCY_GRAPH_H

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace pm_tiny {

struct dependency_node_config {
    std::string name;
    std::vector<std::string> depends_on;
};

enum class dependency_error_code {
    none,
    empty_name,
    duplicate_name,
    duplicate_dependency,
    self_dependency,
    missing_dependency,
    cycle
};

struct dependency_error {
    dependency_error_code code = dependency_error_code::none;
    std::string message;
    std::vector<std::string> cycle_path;
};

class dependency_graph {
public:
    using node_id = std::size_t;
    static const node_id npos;

    static bool build(const std::vector<dependency_node_config> &configs,
                      dependency_graph &result, dependency_error &error);

    std::size_t size() const { return names_.size(); }
    bool empty() const { return names_.empty(); }
    node_id find(const std::string &name) const;
    const std::string &name(node_id id) const { return names_.at(id); }
    const std::vector<node_id> &dependencies(node_id id) const { return dependencies_.at(id); }
    const std::vector<node_id> &dependents(node_id id) const { return dependents_.at(id); }
    const std::vector<node_id> &topological_order() const { return topological_order_; }
    std::vector<node_id> reverse_topological_order() const;
    std::vector<node_id> transitive_dependents(node_id id) const;

private:
    std::vector<std::string> names_;
    std::unordered_map<std::string, node_id> indices_;
    std::vector<std::vector<node_id> > dependencies_;
    std::vector<std::vector<node_id> > dependents_;
    std::vector<node_id> topological_order_;
};

enum class dependency_runtime_state {
    idle,
    pending,
    starting,
    ready,
    failed,
    blocked
};

struct dependency_failure_result {
    std::vector<std::string> failed;
    std::vector<std::string> blocked;
};

class dependency_runtime {
public:
    dependency_runtime() = default;
    explicit dependency_runtime(const dependency_graph &graph);

    void reset(const dependency_graph &graph);
    std::vector<std::string> request_all();
    std::vector<std::string> request_closure(const std::string &name);
    std::vector<std::string> mark_ready(const std::string &name);
    dependency_failure_result mark_failed(const std::string &name);
    void mark_starting(const std::string &name);
    void mark_idle(const std::string &name);

    dependency_runtime_state state(const std::string &name) const;
    std::vector<std::string> blocked_by(const std::string &name) const;
    bool all_terminal() const;

private:
    bool dependencies_ready(dependency_graph::node_id id) const;
    std::vector<std::string> collect_startable();
    void request_dependencies(dependency_graph::node_id id);

    const dependency_graph *graph_ = nullptr;
    std::vector<dependency_runtime_state> states_;
    std::vector<bool> requested_;
};

} // namespace pm_tiny

#endif
