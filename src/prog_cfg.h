//
// Created by qianlinluo@foxmail.com on 23-6-21.
//

#ifndef PM_TINY_PROG_CFG_H
#define PM_TINY_PROG_CFG_H

#include <string>
#include <vector>
#include <iosfwd>
#include "graph.h"
#include <memory>
#include <stdexcept>
#include "pm_tiny_enum.h"
namespace pm_tiny {

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
        int heartbeat_timeout = -1;//The unit is second, its value <=0 means disable
        std::vector<std::string> env_vars;
    };
    using prog_cfg_graph_t = Graph<prog_cfg_t *>;

    std::ostream &operator<<(std::ostream &os, const prog_cfg_t &prog_cfg);

    std::vector<std::string> load_app_environ(const std::string &name,
                                              const std::string &app_environ_dir);

    std::vector<prog_cfg_t> load_prog_cfg(const std::string &cfg_file,
                                          const std::string &app_environ_dir);

    std::unique_ptr<prog_cfg_graph_t> check_prog_cfg(const std::vector<prog_cfg_t> &prog_cfgs);


    int save_prog_cfg(const std::vector<prog_cfg_t> &cfgs,
                      const std::string &cfg_path, const std::string &app_environ_dir);
}
#endif //PM_TINY_PROG_CFG_H
