#include "daemon/prog_cfg.h"
#include "core/log.h"
#include <cstdlib>
#include <iostream>
#include <unordered_map>

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    const char *configured_home = std::getenv("PM_TINY_HOME");
    std::string pm_tiny_home = configured_home != nullptr
                                   ? configured_home
                                   : "/tmp/pm_tiny_test";
    std::string cfg_file = pm_tiny_home + "/prog.cfg";
    std::string env_dir = pm_tiny_home + "/environ";
    pm_tiny::initialize();
    auto prog_cfgs = pm_tiny::load_prog_cfg(cfg_file,
                                            env_dir);
    printf("prog cfg size: %zu\n", prog_cfgs.size());
    for (auto &cfg: prog_cfgs) {
        std::cout << cfg << std::endl;
    }
//    pm_tiny::save_prog_cfg(prog_cfgs, cfg_file, env_dir);
    return 0;
}
