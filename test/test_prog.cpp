//
// Created by qianlinluo@foxmail.com on 23-6-21.
//
#include "../src/prog_cfg.h"
#include "../src/log.h"
#include "../src/graph.h"
#include <iostream>
#include <unordered_map>

int main(int argc, char *argv[]) {
    std::string pm_tiny_home = "/home/sansi/.pm_tiny";
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
    auto graph = pm_tiny::check_prog_cfg(prog_cfgs);
    auto G = *graph;
    if (!graph) {
        std::cout << "invalid cfg" << std::endl;
        return 0;
    }

    return 0;
}