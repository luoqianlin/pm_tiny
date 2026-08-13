#ifndef PM_TINY_PROCESS_LIST_RENDERER_H
#define PM_TINY_PROCESS_LIST_RENDERER_H

#include <cstddef>
#include <string>
#include <vector>

#include "process_list.h"

namespace pm_tiny {
namespace cli {

struct list_render_options {
    bool wide = false;
    bool json = false;
    bool no_color = false;
    bool stdout_is_tty = false;
    std::size_t terminal_width = 0;
};

std::string render_process_list(const std::vector<process_list_entry> &entries,
                                const list_render_options &options);
std::size_t stdout_terminal_width();
bool stdout_supports_color();

} // namespace cli
} // namespace pm_tiny

#endif
