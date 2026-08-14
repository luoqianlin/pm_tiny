
#ifndef PM_TINY_PROG_CFG_H
#define PM_TINY_PROG_CFG_H

#include <string>
#include <vector>
#include <iosfwd>
#include <memory>
#include <stdexcept>
#include "pm_tiny_enum.h"
#include "core/prog_cfg.h"
namespace pm_tiny {
    struct prog_cfg_load_result_t {
        bool success = false;
        std::vector<prog_cfg_t> programs;
        std::string error;
    };

    std::ostream &operator<<(std::ostream &os, const prog_cfg_t &prog_cfg);

    std::vector<std::string> load_app_environ(const std::string &name,
                                              const std::string &app_environ_dir);

    prog_cfg_load_result_t load_prog_cfg(const std::string &cfg_file,
                                         const std::string &app_environ_dir);

    bool recover_prog_cfg_save(const std::string &cfg_file,
                               const std::string &app_environ_dir,
                               std::string &error);

    int save_prog_cfg(const std::vector<prog_cfg_t> &cfgs,
                      const std::string &cfg_path, const std::string &app_environ_dir);
}
#endif //PM_TINY_PROG_CFG_H
