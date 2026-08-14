#ifndef PM_TINY_CONTROL_OPERATION_H
#define PM_TINY_CONTROL_OPERATION_H

#include <cstdint>

namespace pm_tiny {

enum class control_operation_type { stop, restart, remove };

struct control_operation_state {
    bool process_exists = false;
    bool process_active = false;
    bool definition_exists = false;
    std::uint64_t generation = 0;
};

struct control_operation {
    control_operation_type type = control_operation_type::stop;
    std::uint64_t generation = 0;

    bool complete(const control_operation_state &state) const;
};

} // namespace pm_tiny

#endif
