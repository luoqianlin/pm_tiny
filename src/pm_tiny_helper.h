//
// Created by luo on 2021/10/8.
//

#ifndef PM_TINY_PM_TINY_HELPER_H
#define PM_TINY_PM_TINY_HELPER_H

#include <string>
#include <memory>
#include <iosfwd>
namespace pm_tiny {
    struct pm_tiny_config_t {
        std::string pm_tiny_home_dir;
        std::string pm_tiny_lock_file;
        std::string pm_tiny_sock_file;
        std::string pm_tiny_log_file;
        std::string pm_tiny_prog_cfg_file;
        std::string pm_tiny_app_log_dir;
        std::string pm_tiny_app_environ_dir;
        bool uds_abstract_namespace= false;
    };

    std::ostream& operator<<(std::ostream &os, const pm_tiny_config_t &config);
    std::unique_ptr<pm_tiny_config_t> get_file_config(const std::string &cfg_file = "");

    std::unique_ptr<pm_tiny_config_t> get_pm_tiny_config(const std::string &cfg_file = "");


}
#endif //PM_TINY_PM_TINY_HELPER_H
