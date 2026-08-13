#ifndef PM_TINY_CORE_PROG_CFG_H
#define PM_TINY_CORE_PROG_CFG_H

#include "pm_tiny_enum.h"

#include <string>
#include <vector>

namespace pm_tiny {

constexpr int restart_duration_max_ms = 24 * 60 * 60 * 1000;
constexpr int restart_attempts_max = 100000;

struct prog_cfg_t {
    std::string name;
    std::string cwd;
    std::string command;
    int kill_timeout_s = 3;
    std::string run_as;
    std::vector<std::string> envs;
    std::vector<std::string> depends_on;
    int start_timeout = 0;
    failure_action_t failure_action = failure_action_t::SKIP;
    bool daemon = true;
    int heartbeat_timeout = -1;
    std::vector<std::string> env_vars;
    int oom_score_adj = 0;
    bool pty = true;
    int restart_delay_ms = 1000;
    int restart_max_delay_ms = 30000;
    int restart_window_ms = 60000;
    int restart_max_attempts = 10;
    int restart_reset_after_ms = 60000;
};

} // namespace pm_tiny

#endif
