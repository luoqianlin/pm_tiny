#include "termination_job.h"

int main() {
    pm_tiny::termination_job job;
    if (job.request(7, 1000, 1) != pm_tiny::termination_action::send_term) return 1;
    if (job.request(7, 1100, 1) != pm_tiny::termination_action::none) return 2;
    if (job.poll(7, 1500, false) != pm_tiny::termination_action::none) return 3;
    if (job.poll(7, 2000, false) != pm_tiny::termination_action::send_kill) return 4;
    if (job.mark_tree_draining(7, 2100, 1) != pm_tiny::termination_action::none) return 5;
    if (job.poll(6, 3000, true) != pm_tiny::termination_action::none) return 6;
    if (job.poll(7, 3000, true) != pm_tiny::termination_action::complete) return 7;
    if (job.poll(7, 4000, true) != pm_tiny::termination_action::none) return 8;

    if (job.request(8, 5000, 0) != pm_tiny::termination_action::send_term) return 9;
    if (job.mark_tree_draining(8, 5000, 0) != pm_tiny::termination_action::send_term) return 10;
    if (job.force(8) != pm_tiny::termination_action::send_kill) return 11;

    pm_tiny::termination_job orphan_job;
    if (orphan_job.mark_tree_draining(9, 6000, 1) != pm_tiny::termination_action::send_term) return 12;
    if (orphan_job.poll(9, 6999, false) != pm_tiny::termination_action::none) return 13;
    if (orphan_job.poll(9, 7000, false) != pm_tiny::termination_action::send_kill) return 14;
    return 0;
}
