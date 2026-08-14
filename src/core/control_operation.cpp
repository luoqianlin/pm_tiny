#include "control_operation.h"

namespace pm_tiny {

bool control_operation::complete(const control_operation_state &state) const {
    switch (type) {
        case control_operation_type::stop:
            return state.process_exists && !state.process_active;
        case control_operation_type::restart:
            return state.process_exists && state.process_active && state.generation != generation;
        case control_operation_type::remove:
            return !state.process_exists && !state.definition_exists;
    }
    return false;
}

} // namespace pm_tiny
