//
// Created by luo on 2021/10/8.
//
#include <unistd.h>
#include <pwd.h>
#include "pm_tiny_helper.h"
#include <memory>
#include <yaml-cpp/yaml.h>
#include "pm_tiny.h"
//#include <iostream>
namespace pm_tiny {
    std::ostream &operator<<(std::ostream &os, const pm_tiny_config_t &config) {
        os << "home_dir: " << config.pm_tiny_home_dir << std::endl;
        os << "lock_file: " << config.pm_tiny_lock_file << std::endl;
        os << "sock_file: " << config.pm_tiny_sock_file << std::endl;
        os << "log_file: " << config.pm_tiny_log_file << std::endl;
        os << "prog_cfg_file: " << config.pm_tiny_prog_cfg_file << std::endl;
        os << "app_log_dir: " << config.pm_tiny_app_log_dir << std::endl;
        os << "app_environ_dir: " << config.pm_tiny_app_environ_dir << std::endl;
        os << "uds_abstract_namespace: " <<
           (config.uds_abstract_namespace ? "Enable" : "Disable") << std::endl;
        return os;
    }
    static void remove_last_slash(std::string &dir_path) {
        if (dir_path.empty()) { return; }
        if (dir_path[dir_path.length() - 1] == '/') {
            dir_path = dir_path.substr(0, dir_path.length() - 1);
        }
    }

    std::unique_ptr<pm_tiny_config_t> get_file_config(const std::string &cfg_file) {
        std::string file = cfg_file;
        if (file.empty() || access(file.c_str(), F_OK | R_OK) != 0) {
            file = PM_TINY_DEFAULT_CFG_FILE;
        }
        if (access(file.c_str(), F_OK | R_OK) != 0) {
            return nullptr;
        }
        YAML::Node configNode = YAML::LoadFile(file);
        auto s_cfg = std::make_unique<pm_tiny_config_t>();
        std::string pm_tiny_home_dir;
        std::string pm_tiny_log_file;
        std::string pm_tiny_prog_cfg_file;
        std::string pm_tiny_app_log_dir;
        std::string pm_tiny_app_environ_dir;
        std::string pm_tiny_sock_file;
#define ASSIGIN_VARIABLE(key) do{\
        if (configNode[#key]) { key = configNode[#key].as<std::string>();} \
        break;}while(1)

        ASSIGIN_VARIABLE(pm_tiny_home_dir);
        remove_last_slash(pm_tiny_home_dir);
        ASSIGIN_VARIABLE(pm_tiny_log_file);
        ASSIGIN_VARIABLE(pm_tiny_prog_cfg_file);
        ASSIGIN_VARIABLE(pm_tiny_sock_file);
        ASSIGIN_VARIABLE(pm_tiny_app_log_dir);
        ASSIGIN_VARIABLE(pm_tiny_app_environ_dir);
        remove_last_slash(pm_tiny_app_log_dir);
        remove_last_slash(pm_tiny_app_environ_dir);
        const YAML::Node abstract_namespace_node = configNode["pm_tiny_uds_abstract_namespace"];
        if (abstract_namespace_node) {
            s_cfg->uds_abstract_namespace = abstract_namespace_node.as<bool>();
        } else {
#ifdef PM_TINY_UDS_ABSTRACT_NAMESPACE_DEFAULT
            s_cfg->uds_abstract_namespace = true;
#else
            s_cfg->uds_abstract_namespace = false;
#endif
        }
        s_cfg->pm_tiny_home_dir = pm_tiny_home_dir;
        s_cfg->pm_tiny_log_file = pm_tiny_log_file;
        s_cfg->pm_tiny_prog_cfg_file = pm_tiny_prog_cfg_file;
        s_cfg->pm_tiny_app_log_dir = pm_tiny_app_log_dir;
        s_cfg->pm_tiny_app_environ_dir = pm_tiny_app_environ_dir;
        s_cfg->pm_tiny_sock_file = pm_tiny_sock_file;
#undef ASSIGIN_VARIABLE
        return s_cfg;
    }

    static std::string getenv(const std::string &name) {
        auto v = ::getenv(name.c_str());
        if (v == nullptr) {
            return {};
        } else {
            return v;
        }
    }

    std::unique_ptr<pm_tiny_config_t> get_pm_tiny_config(const std::string &cfg_file) {

        std::string pm_tiny_home_dir = getenv(PM_TINY_HOME);
        std::string pm_tiny_log_file = getenv(PM_TINY_LOG_FILE);
        std::string pm_tiny_prog_cfg_file = getenv(PM_TINY_PROG_CFG_FILE);
        std::string pm_tiny_app_log_dir = getenv(PM_TINY_APP_LOG_DIR);
        std::string pm_tiny_app_environ_dir = getenv(PM_TINY_APP_ENVIRON_DIR);
        std::string pm_tiny_sock_file = getenv(PM_TINY_SOCK_FILE);
        std::string pm_tiny_abstract_namespace_str = getenv(PM_TINY_UDS_ABSTRACT_NAMESPACE);
        remove_last_slash(pm_tiny_home_dir);
        remove_last_slash(pm_tiny_app_log_dir);
        remove_last_slash(pm_tiny_app_environ_dir);
        auto f_cfg = get_file_config(cfg_file);

#define REPLEACE_IF_NOT_EMPTY(field)   do {\
        if (field.empty() && f_cfg) {\
            field = f_cfg->field;\
        }} while (false)

        REPLEACE_IF_NOT_EMPTY(pm_tiny_home_dir);
        REPLEACE_IF_NOT_EMPTY(pm_tiny_log_file);
        REPLEACE_IF_NOT_EMPTY(pm_tiny_sock_file);
        REPLEACE_IF_NOT_EMPTY(pm_tiny_prog_cfg_file);
        REPLEACE_IF_NOT_EMPTY(pm_tiny_app_log_dir);
        REPLEACE_IF_NOT_EMPTY(pm_tiny_app_environ_dir);
        bool pm_tiny_uds_abstract_namespace;
        if (!pm_tiny_abstract_namespace_str.empty()) {
            pm_tiny_uds_abstract_namespace =
                    (pm_tiny_abstract_namespace_str == "1"
                     || pm_tiny_abstract_namespace_str == "Y");
        }else {
            if (f_cfg) {
                pm_tiny_uds_abstract_namespace = f_cfg->uds_abstract_namespace;
            }else{
#ifdef PM_TINY_UDS_ABSTRACT_NAMESPACE_DEFAULT
                pm_tiny_uds_abstract_namespace = true;
#else
                pm_tiny_uds_abstract_namespace = false;
#endif
            }
        }
        if (pm_tiny_home_dir.empty()) {
            struct passwd *pw = getpwuid(getuid());
            std::string user_homedir = pw->pw_dir;
            pm_tiny_home_dir = user_homedir + "/.pm_tiny";
        }
        if (pm_tiny_log_file.empty()) {
            pm_tiny_log_file = pm_tiny_home_dir + "/pm_tiny.log";
        }
        if (pm_tiny_prog_cfg_file.empty()) {
            pm_tiny_prog_cfg_file = pm_tiny_home_dir + "/prog.yaml";
        }
        if (pm_tiny_app_log_dir.empty()) {
            pm_tiny_app_log_dir = pm_tiny_home_dir + "/logs";
        }
        if (pm_tiny_app_environ_dir.empty()) {
            pm_tiny_app_environ_dir = pm_tiny_home_dir + "/environ";
        }
        if (pm_tiny_sock_file.empty()) {
            if (pm_tiny_uds_abstract_namespace) {
                pm_tiny_sock_file = "pm_tinyd";
            } else {
                pm_tiny_sock_file = pm_tiny_home_dir + "/pm_tinyd.sock";
            }
        }
        setenv(PM_TINY_HOME, pm_tiny_home_dir.c_str(), 1);
        setenv(PM_TINY_LOG_FILE, pm_tiny_log_file.c_str(), 1);
        setenv(PM_TINY_PROG_CFG_FILE, pm_tiny_prog_cfg_file.c_str(), 1);
        setenv(PM_TINY_APP_LOG_DIR, pm_tiny_app_log_dir.c_str(), 1);
        setenv(PM_TINY_APP_ENVIRON_DIR, pm_tiny_app_environ_dir.c_str(), 1);
        setenv(PM_TINY_SOCK_FILE, pm_tiny_sock_file.c_str(), 1);
        setenv(PM_TINY_UDS_ABSTRACT_NAMESPACE, (pm_tiny_uds_abstract_namespace ? "1" : "0"), 1);
        std::string pm_lock_file = pm_tiny_home_dir + "/" + "pm_tiny.pid";
        auto cfg = std::make_unique<pm_tiny_config_t>();
        cfg->pm_tiny_home_dir = pm_tiny_home_dir;
        cfg->pm_tiny_lock_file = pm_lock_file;
        cfg->pm_tiny_sock_file = pm_tiny_sock_file;
        cfg->pm_tiny_log_file = pm_tiny_log_file;
        cfg->pm_tiny_prog_cfg_file = pm_tiny_prog_cfg_file;
        cfg->pm_tiny_app_log_dir = pm_tiny_app_log_dir;
        cfg->pm_tiny_app_environ_dir = pm_tiny_app_environ_dir;
        cfg->uds_abstract_namespace = pm_tiny_uds_abstract_namespace;
//        std::cout<<*cfg<<std::endl;
        return cfg;
    }
}
