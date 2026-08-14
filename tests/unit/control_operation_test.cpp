#include "control_operation.h"

int main() {
    using pm_tiny::control_operation;
    using pm_tiny::control_operation_state;
    using pm_tiny::control_operation_type;

    control_operation_state running{true, true, true, 7};
    control_operation_state stopped{true, false, true, 7};
    control_operation_state restarted{true, true, true, 8};
    control_operation_state removed{false, false, false, 0};

    if (control_operation{control_operation_type::stop, 7}.complete(running)) return 1;
    if (!control_operation{control_operation_type::stop, 7}.complete(stopped)) return 2;
    if (control_operation{control_operation_type::restart, 7}.complete(stopped)) return 3;
    if (control_operation{control_operation_type::restart, 7}.complete(running)) return 4;
    if (!control_operation{control_operation_type::restart, 7}.complete(restarted)) return 5;
    if (control_operation{control_operation_type::remove, 7}.complete(stopped)) return 6;
    if (!control_operation{control_operation_type::remove, 7}.complete(removed)) return 7;
    return 0;
}
