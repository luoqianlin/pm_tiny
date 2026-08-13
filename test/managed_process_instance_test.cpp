#include "prog.h"

int main() {
    pm_tiny::managed_process_instance instance;
    if (instance.generation != 0 || instance.pid != -1) return 1;

    instance.begin(101);
    const uint64_t first_generation = instance.generation;
    if (first_generation == 0 || instance.pid != 101 || instance.last_pid != 101) return 2;

    instance.termination = pm_tiny::termination_phase::force_kill_requested;
    instance.tree.active = true;
    pm_tiny::generation_task old_task{first_generation, {}};
    instance.begin(102);
    if (instance.generation != first_generation + 1 || instance.pid != 102) return 3;
    if (instance.tree.active || instance.termination != pm_tiny::termination_phase::none) return 4;
    if (old_task.matches(instance.generation)) return 5;

    pm_tiny::generation_task current_task{instance.generation, {}};
    if (!current_task.matches(instance.generation)) return 6;
    const uint64_t failed_generation = instance.generation;
    instance.reset_failed_start();
    if (instance.generation != failed_generation || instance.pid != -1 || instance.last_pid != -1) return 7;
    return 0;
}
