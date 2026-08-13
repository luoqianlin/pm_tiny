#include "prog_cfg.h"
#include "log.h"
#include "string_utils.h"
#include "core/prog_cfg_yaml_helper.h"
#include "core/prog_cfg_order.h"
#include <fstream>
#include <yaml-cpp/yaml.h>
#include <ostream>
#include <unordered_map>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>

namespace {
bool path_exists(const std::string &path) { return access(path.c_str(), F_OK) == 0; }

void remove_flat_directory(const std::string &path) {
    DIR *dir = opendir(path.c_str());
    if (!dir) return;
    while (auto *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        unlink((path + "/" + name).c_str());
    }
    closedir(dir);
    rmdir(path.c_str());
}
}

namespace pm_tiny {
    std::ostream &operator<<(std::ostream &os, const prog_cfg_t &prog_cfg) {
        os << "=== " << prog_cfg.name << " ===" << std::endl;
        os << "command:" << prog_cfg.command << std::endl;
        os << "cwd:" << prog_cfg.cwd << std::endl;
        os << "kill_timeout_s:" << prog_cfg.kill_timeout_s << std::endl;
        os << "run_as:" << prog_cfg.run_as << std::endl;
        os << "depends_on:[";
        for (int i = 0; i < static_cast<int>(prog_cfg.depends_on.size()); i++) {
            const auto &c = prog_cfg.depends_on[i];
            os << c;
            if (i != static_cast<int>(prog_cfg.depends_on.size()) - 1) {
                os << ",";
            }
        }
        os << "]";
        os << std::endl;
        os << "start_timeout:" << prog_cfg.start_timeout << std::endl;
        os << "failure_action:" << prog_cfg.failure_action << std::endl;
        os << "daemon:" << std::boolalpha << prog_cfg.daemon << std::endl;
        os << "heartbeat_timeout:" << prog_cfg.heartbeat_timeout << std::endl;
        os << "oom_score_adj:" << prog_cfg.oom_score_adj << std::endl;
        os << "pty:" << std::boolalpha << prog_cfg.pty;
        return os;
    }

    int save_prog_cfg(const std::vector<prog_cfg_t> &cfgs,
                      const std::string &cfg_path,
                      const std::string &app_environ_dir) {
//        std::stringstream ss;
        std::vector<std::tuple<std::string, std::string>> f_envs;
        YAML::Node rootNode;
        for (const auto &cfg: cfgs) {
//            ss << cfg.name << ":" << cfg.cwd << ":" << cfg.command
//               << ":" << cfg.kill_timeout_s << ":" << cfg.run_as << "\n";
            YAML::Node prop = serialize_prog_cfg_yaml_node(cfg);
            rootNode.push_back(prop);
            std::stringstream env_ss;
            for (auto &env: cfg.envs) {
                env_ss << env << "\n";
            }
            f_envs.emplace_back(cfg.name, env_ss.str());
        }
        const auto suffix = ".txn." + std::to_string(static_cast<long long>(getpid()));
        const std::string tmp_path = cfg_path + suffix;
        const std::string cfg_backup = cfg_path + ".bak" + suffix;
        const std::string env_stage = app_environ_dir + suffix;
        const std::string env_backup = app_environ_dir + ".bak" + suffix;
        remove_flat_directory(env_stage);
        remove_flat_directory(env_backup);
        unlink(cfg_backup.c_str());
        if (mkdir(env_stage.c_str(), 0700) != 0) {
            PM_TINY_LOG_E("cannot create temporary environ dir %s", env_stage.c_str());
            return -1;
        }
        for (auto &env: f_envs) {
            const auto filename = env_stage + "/" + std::get<0>(env);
            std::fstream env_fs(filename, std::ios::out | std::ios::trunc);
            if (!env_fs) {
                remove_flat_directory(env_stage);
                PM_TINY_LOG_E("%s write fail", std::get<0>(env).c_str());
                return -1;
            }
            env_fs << std::get<1>(env);
            env_fs.flush();
            if (!env_fs.good()) {
                env_fs.close();
                remove_flat_directory(env_stage);
                return -1;
            }
        }
        std::fstream cfg_file(tmp_path, std::ios::out | std::ios::trunc);
        if (!cfg_file) {
            remove_flat_directory(env_stage);
            PM_TINY_LOG_E("cannot open temporary cfg %s", tmp_path.c_str());
            return -1;
        }
//        cfg_file << ss.str();
        if (cfgs.empty()) {
            cfg_file << "[]\n";
        } else {
            cfg_file << rootNode;
        }
        cfg_file.flush();
        if (!cfg_file.good()) {
            cfg_file.close();
            unlink(tmp_path.c_str());
            remove_flat_directory(env_stage);
            PM_TINY_LOG_E("cannot write cfg %s", tmp_path.c_str());
            return -1;
        }
        cfg_file.close();
        const bool had_cfg = path_exists(cfg_path);
        const bool had_env = path_exists(app_environ_dir);
        if (had_cfg && rename(cfg_path.c_str(), cfg_backup.c_str()) != 0) {
            unlink(tmp_path.c_str());
            remove_flat_directory(env_stage);
            return -1;
        }
        if (had_env && rename(app_environ_dir.c_str(), env_backup.c_str()) != 0) {
            if (had_cfg) rename(cfg_backup.c_str(), cfg_path.c_str());
            unlink(tmp_path.c_str());
            remove_flat_directory(env_stage);
            return -1;
        }
        if (rename(tmp_path.c_str(), cfg_path.c_str()) != 0 ||
            rename(env_stage.c_str(), app_environ_dir.c_str()) != 0) {
            unlink(cfg_path.c_str());
            remove_flat_directory(app_environ_dir);
            if (had_cfg) rename(cfg_backup.c_str(), cfg_path.c_str());
            if (had_env) rename(env_backup.c_str(), app_environ_dir.c_str());
            unlink(tmp_path.c_str());
            remove_flat_directory(env_stage);
            return -1;
        }
        unlink(cfg_backup.c_str());
        remove_flat_directory(env_backup);
        return 0;
    }

    std::vector<std::string> load_app_environ(const std::string &name,
                                              const std::string &app_environ_dir) {
        std::vector<std::string> envs;
        std::fstream efs(app_environ_dir + "/" + name);
        if (!efs) {
            PM_TINY_LOG_D("%s environ not exists", name.c_str());
            for (char **env = environ; *env != nullptr; env++) {
                envs.emplace_back(*env);
            }
            return envs;
        }

        for (std::string line; std::getline(efs, line);) {
            mgr::utils::trim(line);
            if (line.empty())continue;
            envs.emplace_back(line);
        }
        return envs;
    }

    std::vector<prog_cfg_t> load_prog_cfg_0(const std::string &cfg_path,
                                            const std::string &app_environ_dir) {
        std::fstream cfg_file(cfg_path);
        if (!cfg_file) {
            PM_TINY_LOG_E("not found cfg:%s", cfg_path.c_str());
            return {};
        }
        std::vector<prog_cfg_t> cfgs;
        for (std::string line; std::getline(cfg_file, line);) {
            mgr::utils::trim(line);
            if (!line.empty() && line[0] != '#') {
                auto elements = mgr::utils::split(line, {':'});
                if (elements.size() < 3) {
                    continue;
                }
                for (auto &v: elements) {
                    mgr::utils::trim(v);
                }
                auto &app_name = elements[0];

                int kill_timeout_s = 3;
                std::string run_as;
                if (elements.size() > 3) {
                    try {
                        kill_timeout_s = std::stoi(elements[3]);
                    } catch (const std::exception &ex) {
                        //ignore
                    }
                    if (elements.size() > 4) {
                        run_as = elements[4];
                    }
                }
                prog_cfg_t prog_cfg;
                prog_cfg.name = app_name;
                prog_cfg.run_as = run_as;
                prog_cfg.kill_timeout_s = kill_timeout_s;
                prog_cfg.cwd = elements[1];
                prog_cfg.command = elements[2];
                prog_cfg.envs = load_app_environ(app_name, app_environ_dir);
                cfgs.push_back(prog_cfg);
            }
        }
        return cfgs;
    }

    std::vector<prog_cfg_t> load_prog_cfg_yaml(const std::string &cfg_path,
                                          const std::string &app_environ_dir) {
        YAML::Node progNodes;
        try {
            progNodes = YAML::LoadFile(cfg_path);
        } catch (const YAML::Exception &ex) {
            PM_TINY_LOG_E("Failed to load cfg %s: %s", cfg_path.c_str(), ex.what());
            return {};
        }

        auto document = parse_prog_cfg_yaml_document(progNodes);
        for (const auto &warning : document.warnings) PM_TINY_LOG_E("%s", warning.c_str());
        if (!document.success) {
            PM_TINY_LOG_E("cfg file %s invalid: %s", cfg_path.c_str(), document.error.c_str());
            return {};
        }
        for (auto &cfg : document.programs) {
            cfg.envs = load_app_environ(cfg.name, app_environ_dir);
        }
        return document.programs;
    }


    std::vector<prog_cfg_t> load_prog_cfg(const std::string &cfg_path,
                                          const std::string &app_environ_dir) {
        std::string name, ext;
        std::tie(name, ext) = mgr::utils::splitext(cfg_path);
        auto is_yaml = (ext == ".yaml" || ext == ".yml");
        if (!is_yaml) {
            auto yam_cfg_path = name + ".yaml";
            if (access(yam_cfg_path.c_str(), F_OK) == 0) {
                PM_TINY_LOG_I("found %s file with the same name use it",yam_cfg_path.c_str());
                return load_prog_cfg_yaml(yam_cfg_path, app_environ_dir);
            }else{
                return load_prog_cfg_0(cfg_path, app_environ_dir);
            }
        } else {
            if (access(cfg_path.c_str(), F_OK) != 0) {
                PM_TINY_LOG_E("not found cfg:%s", cfg_path.c_str());
                std::string old_cfg_path = name + ".cfg";
                if (access(old_cfg_path.c_str(), F_OK | R_OK) == 0) {//try upgrade to yaml
                    PM_TINY_LOG_I("Found the configuration file %s in the old format "
                                  "and used it as the current configuration", old_cfg_path.c_str());
                    auto prog_cfgs = load_prog_cfg_0(old_cfg_path, app_environ_dir);
                    PM_TINY_LOG_I("Convert the old configuration file to %s", cfg_path.c_str());
                    save_prog_cfg(prog_cfgs, cfg_path, app_environ_dir);
                    return prog_cfgs;
                }
                return {};
            }
            return load_prog_cfg_yaml(cfg_path, app_environ_dir);
        }
    }

}
