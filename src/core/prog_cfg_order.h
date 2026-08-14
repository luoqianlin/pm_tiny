#ifndef PM_TINY_PROG_CFG_ORDER_H
#define PM_TINY_PROG_CFG_ORDER_H

#include "dependency_graph.h"
#include "program_name.h"

#include <string>
#include <vector>

namespace pm_tiny {

template <typename Config>
bool validate_and_order_prog_cfgs(std::vector<Config> &programs, std::string &error_message) {
    std::vector<dependency_node_config> configs;
    configs.reserve(programs.size());
    for (const auto &program : programs) {
        if (!is_valid_program_name(program.name)) {
            error_message = program_name_validation_error(program.name);
            return false;
        }
        for (const auto &dependency : program.depends_on) {
            if (!is_valid_program_name(dependency)) {
                error_message = "Program `" + program.name + "` has invalid dependency name: " +
                                program_name_validation_error(dependency);
                return false;
            }
        }
        configs.push_back({program.name, program.depends_on});
    }
    dependency_graph graph;
    dependency_error error;
    if (!dependency_graph::build(configs, graph, error)) {
        error_message = error.message;
        return false;
    }
    std::vector<Config> ordered;
    ordered.reserve(programs.size());
    for (const auto id : graph.topological_order()) ordered.push_back(programs[id]);
    programs.swap(ordered);
    error_message.clear();
    return true;
}

} // namespace pm_tiny

#endif
