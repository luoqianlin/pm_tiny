#ifndef PM_TINY_DEPENDENCY_GRAPH_RENDERER_H
#define PM_TINY_DEPENDENCY_GRAPH_RENDERER_H

#include "process_list.h"

#include <string>
#include <vector>

namespace pm_tiny {
namespace cli {

struct dependency_graph_render_options {
    std::string focus;
    bool json = false;
    bool dot = false;
    bool no_color = false;
    bool stdout_is_tty = false;
};

std::string render_dependency_graph(const std::vector<process_list_entry> &entries,
                                    const dependency_graph_render_options &options = {});

} // namespace cli
} // namespace pm_tiny

#endif
