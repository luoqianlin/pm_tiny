//
// Created by luo on 2021/10/8.
//
#include <unistd.h>
#include <pwd.h>
#include "pm_tiny_helper.h"
#include <memory>
#include <cstdlib>
#include <stdexcept>
#include <yaml-cpp/yaml.h>
#include "pm_tiny.h"
//#include <iostream>
namespace pm_tiny {
    std::ostream &operator<<(std::ostream &os, const pm_tiny_config_t &config) {
        os << "home_dir: " << config.pm_tiny_home_dir << std::endl;
        os << "lock_file: " << config.pm_tiny_lock_file << std::endl;
        os << "sock_file: " << config.pm_tiny_sock_file << std::endl;
        os << "log_file: " << config.pm_tiny_log_file << std::endl;
        os << "log_level: " << config.log_level << std::endl;
        os << "log_max_size_kb: " << config.log_max_size_kb << std::endl;
        os << "log_archive_count: " << config.log_archive_count << std::endl;
        os << "prog_cfg_file: " << config.pm_tiny_prog_cfg_file << std::endl;
        os << "app_log_dir: " << config.pm_tiny_app_log_dir << std::endl;
        os << "app_environ_dir: " << config.pm_tiny_app_environ_dir << std::endl;
        os << "uds_abstract_namespace: " <<
           (config.uds_abstract_namespace ? "Enable" : "Disable") << std::endl;
        os << "process_tree_mode: " << config.process_tree_mode << std::endl;
        os << "cgroup_root: " << config.cgroup_root << std::endl;
        return os;
    }
    static void remove_last_slash(std::string &dir_path) {
        if (dir_path.empty()) { return; }
        if (dir_path[dir_path.length() - 1] == '/') {
            dir_path = dir_path.substr(0, dir_path.length() - 1);
        }
    }

    static void resolve_config_relative_path(const std::string &config_file,
                                             std::string &path) {
        if (path.empty() || path.front() == '/') return;
        const auto slash = config_file.find_last_of('/');
        if (slash != std::string::npos) path = config_file.substr(0, slash + 1) + path;
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
        if (configNode["pm_tiny_process_tree_mode"]) {
            s_cfg->process_tree_mode = configNode["pm_tiny_process_tree_mode"].as<std::string>();
        }
        if (configNode["pm_tiny_cgroup_root"]) {
            s_cfg->cgroup_root = configNode["pm_tiny_cgroup_root"].as<std::string>();
        }
        if (configNode["pm_tiny_log_level"]) {
            s_cfg->log_level = configNode["pm_tiny_log_level"].as<std::string>();
            if (s_cfg->log_level != "debug" && s_cfg->log_level != "info" &&
                s_cfg->log_level != "warn" && s_cfg->log_level != "error" &&
                s_cfg->log_level != "fatal") {
                throw std::runtime_error("pm_tiny_log_level must be debug, info, warn, error, or fatal");
            }
        }
        if (configNode["pm_tiny_log_max_size_kb"]) {
            s_cfg->log_max_size_kb = configNode["pm_tiny_log_max_size_kb"].as<int>();
            if (s_cfg->log_max_size_kb < 1 || s_cfg->log_max_size_kb > 1048576) {
                throw std::runtime_error("pm_tiny_log_max_size_kb must be between 1 and 1048576");
            }
        }
        if (configNode["pm_tiny_log_archive_count"]) {
            s_cfg->log_archive_count = configNode["pm_tiny_log_archive_count"].as<int>();
            if (s_cfg->log_archive_count < 0 || s_cfg->log_archive_count > 100) {
                throw std::runtime_error("pm_tiny_log_archive_count must be between 0 and 100");
            }
        }
        if (configNode["pm_tiny_allowed_uids"])
            s_cfg->allowed_uids = configNode["pm_tiny_allowed_uids"].as<std::vector<unsigned int>>();
        if (configNode["pm_tiny_allowed_gids"])
            s_cfg->allowed_gids = configNode["pm_tiny_allowed_gids"].as<std::vector<unsigned int>>();
        resolve_config_relative_path(file, pm_tiny_home_dir);
        resolve_config_relative_path(file, pm_tiny_log_file);
        resolve_config_relative_path(file, pm_tiny_prog_cfg_file);
        resolve_config_relative_path(file, pm_tiny_app_log_dir);
        resolve_config_relative_path(file, pm_tiny_app_environ_dir);
        if (!s_cfg->uds_abstract_namespace) resolve_config_relative_path(file, pm_tiny_sock_file);
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
        std::string process_tree_mode = getenv(PM_TINY_PROCESS_TREE_MODE);
        std::string cgroup_root = getenv(PM_TINY_CGROUP_ROOT);
        std::string log_level = getenv(PM_TINY_LOG_LEVEL);
        std::string log_max_size_kb = getenv("PM_TINY_LOG_MAX_SIZE_KB");
        std::string log_archive_count = getenv("PM_TINY_LOG_ARCHIVE_COUNT");
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
        if (process_tree_mode.empty() && f_cfg) process_tree_mode = f_cfg->process_tree_mode;
        if (cgroup_root.empty() && f_cfg) cgroup_root = f_cfg->cgroup_root;
        const bool valid_log_level = log_level == "debug" || log_level == "info" ||
            log_level == "warn" || log_level == "error" || log_level == "fatal";
        if (!valid_log_level && f_cfg) log_level = f_cfg->log_level;
        if (log_level != "debug" && log_level != "info" && log_level != "warn" &&
            log_level != "error" && log_level != "fatal") log_level = "info";
        int parsed_log_max_size_kb = f_cfg ? f_cfg->log_max_size_kb : 4096;
        int parsed_log_archive_count = f_cfg ? f_cfg->log_archive_count : 3;
        if (!log_max_size_kb.empty()) {
            const long parsed = std::strtol(log_max_size_kb.c_str(), nullptr, 10);
            if (parsed >= 1 && parsed <= 1048576) parsed_log_max_size_kb = static_cast<int>(parsed);
        }
        if (!log_archive_count.empty()) {
            const long parsed = std::strtol(log_archive_count.c_str(), nullptr, 10);
            if (parsed >= 0 && parsed <= 100) parsed_log_archive_count = static_cast<int>(parsed);
        }
        if (process_tree_mode.empty()) process_tree_mode = "auto";
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
        setenv(PM_TINY_LOG_LEVEL, log_level.c_str(), 1);
        setenv("PM_TINY_LOG_MAX_SIZE_KB", std::to_string(parsed_log_max_size_kb).c_str(), 1);
        setenv("PM_TINY_LOG_ARCHIVE_COUNT", std::to_string(parsed_log_archive_count).c_str(), 1);
        std::string pm_lock_file = pm_tiny_home_dir + "/" + "pm_tiny.pid";
        auto cfg = std::make_unique<pm_tiny_config_t>();
        cfg->pm_tiny_home_dir = pm_tiny_home_dir;
        cfg->pm_tiny_lock_file = pm_lock_file;
        cfg->pm_tiny_sock_file = pm_tiny_sock_file;
        cfg->pm_tiny_log_file = pm_tiny_log_file;
        cfg->log_level = log_level;
        cfg->log_max_size_kb = parsed_log_max_size_kb;
        cfg->log_archive_count = parsed_log_archive_count;
        cfg->pm_tiny_prog_cfg_file = pm_tiny_prog_cfg_file;
        cfg->pm_tiny_app_log_dir = pm_tiny_app_log_dir;
        cfg->pm_tiny_app_environ_dir = pm_tiny_app_environ_dir;
        cfg->uds_abstract_namespace = pm_tiny_uds_abstract_namespace;
        cfg->process_tree_mode = process_tree_mode;
        cfg->cgroup_root = cgroup_root;
        if (f_cfg) {
            cfg->allowed_uids = f_cfg->allowed_uids;
            cfg->allowed_gids = f_cfg->allowed_gids;
        }
//        std::cout<<*cfg<<std::endl;
        return cfg;
    }
}
