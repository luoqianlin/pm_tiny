#include "dependency_graph.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <set>
#include <unordered_set>

namespace pm_tiny {

const dependency_graph::node_id dependency_graph::npos = static_cast<node_id>(-1);

namespace {

std::string cycle_message(const std::vector<std::string> &path) {
    std::string message = "Program dependency cycle detected";
    if (path.empty()) return message;
    message += ": ";
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (i != 0) message += " -> ";
        message += path[i];
    }
    return message;
}

} // namespace

bool dependency_graph::build(const std::vector<dependency_node_config> &configs,
                             dependency_graph &result, dependency_error &error) {
    dependency_graph graph;
    error = dependency_error{};
    graph.names_.reserve(configs.size());
    for (std::size_t i = 0; i < configs.size(); ++i) {
        if (configs[i].name.empty()) {
            error.code = dependency_error_code::empty_name;
            error.message = "Program name cannot be empty";
            return false;
        }
        if (!graph.indices_.emplace(configs[i].name, i).second) {
            error.code = dependency_error_code::duplicate_name;
            error.message = "Duplicate program name: " + configs[i].name;
            return false;
        }
        graph.names_.push_back(configs[i].name);
    }

    graph.dependencies_.resize(configs.size());
    graph.dependents_.resize(configs.size());
    std::vector<std::size_t> indegree(configs.size(), 0);
    for (std::size_t i = 0; i < configs.size(); ++i) {
        std::unordered_set<std::string> seen;
        for (const auto &dependency : configs[i].depends_on) {
            if (!seen.insert(dependency).second) {
                error.code = dependency_error_code::duplicate_dependency;
                error.message = "Program `" + configs[i].name + "` has duplicate dependency `" + dependency + "`";
                return false;
            }
            if (dependency == configs[i].name) {
                error.code = dependency_error_code::self_dependency;
                error.message = "Program `" + configs[i].name + "` cannot depend on itself";
                return false;
            }
            const auto found = graph.indices_.find(dependency);
            if (found == graph.indices_.end()) {
                error.code = dependency_error_code::missing_dependency;
                error.message = "Program `" + configs[i].name + "` depends on missing `" + dependency + "`";
                return false;
            }
            graph.dependencies_[i].push_back(found->second);
            graph.dependents_[found->second].push_back(i);
            ++indegree[i];
        }
    }

    std::priority_queue<node_id, std::vector<node_id>, std::greater<node_id> > ready;
    for (node_id i = 0; i < configs.size(); ++i) if (indegree[i] == 0) ready.push(i);
    while (!ready.empty()) {
        const auto id = ready.top();
        ready.pop();
        graph.topological_order_.push_back(id);
        for (const auto dependent : graph.dependents_[id]) {
            if (--indegree[dependent] == 0) ready.push(dependent);
        }
    }
    if (graph.topological_order_.size() != configs.size()) {
        std::vector<int> color(configs.size(), 0);
        std::vector<node_id> stack;
        std::function<bool(node_id)> visit = [&](node_id id) {
            color[id] = 1;
            stack.push_back(id);
            for (const auto dependency : graph.dependencies_[id]) {
                if (color[dependency] == 0 && visit(dependency)) return true;
                if (color[dependency] == 1) {
                    const auto begin = std::find(stack.begin(), stack.end(), dependency);
                    for (auto it = begin; it != stack.end(); ++it) error.cycle_path.push_back(graph.names_[*it]);
                    error.cycle_path.push_back(graph.names_[dependency]);
                    return true;
                }
            }
            stack.pop_back();
            color[id] = 2;
            return false;
        };
        for (node_id i = 0; i < configs.size() && error.cycle_path.empty(); ++i) {
            if (color[i] == 0) visit(i);
        }
        error.code = dependency_error_code::cycle;
        error.message = cycle_message(error.cycle_path);
        return false;
    }
    result = std::move(graph);
    return true;
}

dependency_graph::node_id dependency_graph::find(const std::string &name_value) const {
    const auto found = indices_.find(name_value);
    return found == indices_.end() ? npos : found->second;
}

std::vector<dependency_graph::node_id> dependency_graph::reverse_topological_order() const {
    return std::vector<node_id>(topological_order_.rbegin(), topological_order_.rend());
}

std::vector<dependency_graph::node_id> dependency_graph::transitive_dependents(node_id id) const {
    std::vector<node_id> result;
    std::vector<bool> visited(size(), false);
    std::queue<node_id> pending;
    pending.push(id);
    visited[id] = true;
    while (!pending.empty()) {
        const auto current = pending.front();
        pending.pop();
        for (const auto dependent : dependents_[current]) {
            if (!visited[dependent]) {
                visited[dependent] = true;
                result.push_back(dependent);
                pending.push(dependent);
            }
        }
    }
    std::sort(result.begin(), result.end(), [&](node_id left, node_id right) {
        const auto left_pos = std::find(topological_order_.begin(), topological_order_.end(), left);
        const auto right_pos = std::find(topological_order_.begin(), topological_order_.end(), right);
        return left_pos < right_pos;
    });
    return result;
}

dependency_runtime::dependency_runtime(const dependency_graph &graph) { reset(graph); }

void dependency_runtime::reset(const dependency_graph &graph) {
    graph_ = &graph;
    states_.assign(graph.size(), dependency_runtime_state::idle);
    requested_.assign(graph.size(), false);
}

void dependency_runtime::request_dependencies(dependency_graph::node_id id) {
    if (requested_[id] && states_[id] != dependency_runtime_state::failed &&
        states_[id] != dependency_runtime_state::blocked) return;
    requested_[id] = true;
    if (states_[id] == dependency_runtime_state::idle ||
        states_[id] == dependency_runtime_state::blocked ||
        states_[id] == dependency_runtime_state::failed)
        states_[id] = dependency_runtime_state::pending;
    for (const auto dependency : graph_->dependencies(id)) request_dependencies(dependency);
}

std::vector<std::string> dependency_runtime::request_all() {
    std::fill(requested_.begin(), requested_.end(), true);
    for (std::size_t i = 0; i < states_.size(); ++i) {
        if (states_[i] == dependency_runtime_state::idle || states_[i] == dependency_runtime_state::blocked)
            states_[i] = dependency_runtime_state::pending;
    }
    return collect_startable();
}

std::vector<std::string> dependency_runtime::request_closure(const std::string &name) {
    const auto id = graph_->find(name);
    if (id == dependency_graph::npos) return {};
    request_dependencies(id);
    return collect_startable();
}

bool dependency_runtime::dependencies_ready(dependency_graph::node_id id) const {
    for (const auto dependency : graph_->dependencies(id)) {
        if (states_[dependency] != dependency_runtime_state::ready) return false;
    }
    return true;
}

std::vector<std::string> dependency_runtime::collect_startable() {
    std::vector<std::string> result;
    for (const auto id : graph_->topological_order()) {
        if (requested_[id] && states_[id] == dependency_runtime_state::pending && dependencies_ready(id)) {
            states_[id] = dependency_runtime_state::starting;
            result.push_back(graph_->name(id));
        }
    }
    return result;
}

std::vector<std::string> dependency_runtime::mark_ready(const std::string &name) {
    const auto id = graph_->find(name);
    if (id == dependency_graph::npos) return {};
    requested_[id] = true;
    states_[id] = dependency_runtime_state::ready;
    for (const auto dependent : graph_->transitive_dependents(id)) {
        if (!requested_[dependent] || states_[dependent] != dependency_runtime_state::blocked) continue;
        bool still_blocked = false;
        for (const auto dependency : graph_->dependencies(dependent)) {
            if (states_[dependency] == dependency_runtime_state::failed ||
                states_[dependency] == dependency_runtime_state::blocked) {
                still_blocked = true;
                break;
            }
        }
        if (!still_blocked) states_[dependent] = dependency_runtime_state::pending;
    }
    return collect_startable();
}

dependency_failure_result dependency_runtime::mark_failed(const std::string &name) {
    dependency_failure_result result;
    const auto id = graph_->find(name);
    if (id == dependency_graph::npos) return result;
    requested_[id] = true;
    states_[id] = dependency_runtime_state::failed;
    result.failed.push_back(name);
    for (const auto dependent : graph_->transitive_dependents(id)) {
        if (!requested_[dependent] || states_[dependent] == dependency_runtime_state::ready) continue;
        if (states_[dependent] != dependency_runtime_state::blocked) result.blocked.push_back(graph_->name(dependent));
        states_[dependent] = dependency_runtime_state::blocked;
    }
    return result;
}

void dependency_runtime::mark_starting(const std::string &name) {
    const auto id = graph_->find(name);
    if (id != dependency_graph::npos) {
        requested_[id] = true;
        states_[id] = dependency_runtime_state::starting;
    }
}

void dependency_runtime::mark_idle(const std::string &name) {
    const auto id = graph_->find(name);
    if (id != dependency_graph::npos) {
        requested_[id] = false;
        states_[id] = dependency_runtime_state::idle;
    }
}

void dependency_runtime::mark_pending(const std::string &name) {
    const auto id = graph_ ? graph_->find(name) : dependency_graph::npos;
    if (id != dependency_graph::npos) {
        requested_[id] = true;
        states_[id] = dependency_runtime_state::pending;
    }
}

dependency_runtime_state dependency_runtime::state(const std::string &name) const {
    const auto id = graph_ ? graph_->find(name) : dependency_graph::npos;
    return id == dependency_graph::npos ? dependency_runtime_state::idle : states_[id];
}

std::vector<std::string> dependency_runtime::blocked_by(const std::string &name) const {
    std::vector<std::string> result;
    const auto id = graph_ ? graph_->find(name) : dependency_graph::npos;
    if (id == dependency_graph::npos) return result;
    std::vector<bool> visited(graph_->size(), false);
    std::function<void(dependency_graph::node_id)> visit = [&](dependency_graph::node_id current) {
        for (const auto dependency : graph_->dependencies(current)) {
            if (visited[dependency]) continue;
            visited[dependency] = true;
            if (states_[dependency] == dependency_runtime_state::failed)
                result.push_back(graph_->name(dependency));
            else if (states_[dependency] == dependency_runtime_state::blocked)
                visit(dependency);
        }
    };
    visit(id);
    std::sort(result.begin(), result.end(), [&](const std::string &left, const std::string &right) {
        return graph_->find(left) < graph_->find(right);
    });
    return result;
}

std::vector<std::string> dependency_runtime::waiting_for(const std::string &name) const {
    std::vector<std::string> result;
    const auto id = graph_ ? graph_->find(name) : dependency_graph::npos;
    if (id == dependency_graph::npos) return result;
    std::vector<bool> visited(graph_->size(), false);
    std::function<void(dependency_graph::node_id)> visit = [&](dependency_graph::node_id current) {
        for (const auto dependency : graph_->dependencies(current)) {
            if (visited[dependency]) continue;
            visited[dependency] = true;
            if (states_[dependency] != dependency_runtime_state::ready)
                result.push_back(graph_->name(dependency));
            visit(dependency);
        }
    };
    visit(id);
    std::sort(result.begin(), result.end(), [&](const std::string &left, const std::string &right) {
        return graph_->find(left) < graph_->find(right);
    });
    return result;
}

bool dependency_runtime::all_terminal() const {
    for (std::size_t i = 0; i < states_.size(); ++i) {
        if (requested_[i] && (states_[i] == dependency_runtime_state::pending ||
                              states_[i] == dependency_runtime_state::starting)) return false;
    }
    return true;
}

std::vector<dependency_runtime_entry> dependency_runtime::snapshot() const {
    std::vector<dependency_runtime_entry> result;
    if (!graph_) return result;
    result.reserve(graph_->size());
    for (std::size_t i = 0; i < graph_->size(); ++i) {
        result.push_back({graph_->name(i), states_[i], requested_[i]});
    }
    return result;
}

void dependency_runtime::migrate(const dependency_graph &graph,
                                 const std::vector<dependency_runtime_entry> &entries) {
    reset(graph);
    for (const auto &entry : entries) {
        const auto id = graph.find(entry.name);
        if (id == dependency_graph::npos) continue;
        states_[id] = entry.state;
        requested_[id] = entry.requested;
    }
}

} // namespace pm_tiny
