//
// Created by qianlinluo@foxmail.com on 23-6-21.
//
#include "prog_cfg.h"
#include "log.h"
#include "string_utils.h"
#include <fstream>
#include <yaml-cpp/yaml.h>
#include <ostream>
#include <unordered_map>

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
        os << "oom_score_adj:" << prog_cfg.oom_score_adj;
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
            YAML::Node prop;
            prop["name"] = cfg.name;
            prop["cwd"] = cfg.cwd;
            prop["command"] = cfg.command;
            prop["env_vars"] = cfg.env_vars;
            prop["kill_timeout_s"] = cfg.kill_timeout_s;
            prop["user"] = cfg.run_as;
            prop["depends_on"] = cfg.depends_on;
            prop["start_timeout"] = cfg.start_timeout;
            prop["failure_action"] = failure_action_to_str(cfg.failure_action);
            prop["daemon"] = cfg.daemon;
            prop["heartbeat_timeout"] = cfg.heartbeat_timeout;
#ifdef __ANDROID__
            prop["oom_score_adj"] = cfg.oom_score_adj;
#endif
            rootNode.push_back(prop);
            std::stringstream env_ss;
            for (auto &env: cfg.envs) {
                env_ss << env << "\n";
            }
            f_envs.emplace_back(cfg.name, env_ss.str());
        }
        std::fstream cfg_file(cfg_path, std::ios::out | std::ios::trunc);
        if (!cfg_file) {
            PM_TINY_LOG_E("not found cfg");
            return -1;
        }
//        cfg_file << ss.str();
        cfg_file << rootNode;
        for (auto &env: f_envs) {
            std::string f_name = std::get<0>(env);
            std::string content = std::get<1>(env);
            auto filename = app_environ_dir;
            filename += "/";
            filename += f_name;
            std::fstream env_fs(filename, std::ios::out | std::ios::trunc);
            if (!env_fs) {
                PM_TINY_LOG_D("%s write fail", f_name.c_str());
            }
            env_fs << content;
        }
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

        YAML::Node progNodes = YAML::LoadFile(cfg_path);
        std::vector<prog_cfg_t> prog_cfgs;
        if (progNodes) {
            for (auto &&prop: progNodes) {
                prog_cfg_t prog_cfg;
                prog_cfg.name = prop["name"].as<std::string>();
                prog_cfg.cwd = prop["cwd"].as<std::string>();
                prog_cfg.command = prop["command"].as<std::string>();
                if (prop["kill_timeout_s"]) {
                    prog_cfg.kill_timeout_s = prop["kill_timeout_s"].as<int>();
                }
                if (prop["user"]) {
                    prog_cfg.run_as = prop["user"].as<std::string>();
                }
                auto depends_on_node = prop["depends_on"];
                if (depends_on_node) {
                    if (depends_on_node.IsScalar()) {
                        prog_cfg.depends_on.push_back(depends_on_node.as<std::string>());
                    } else if (depends_on_node.IsSequence()) {
                        for (auto &&n: depends_on_node) {
                            prog_cfg.depends_on.push_back(n.as<std::string>());
                        }
                    } else {
                        PM_TINY_LOG_E("%s depends_on wrong type, must be a scalar or sequence",
                                      prog_cfg.name.c_str());
                    }
                }
                auto env_vars_node = prop["env_vars"];
                if (env_vars_node) {
                    if (env_vars_node.IsScalar()) {
                        prog_cfg.env_vars.push_back(env_vars_node.as<std::string>());
                    } else if (env_vars_node.IsSequence()) {
                        for (auto &&n: env_vars_node) {
                            prog_cfg.env_vars.push_back(n.as<std::string>());
                        }
                    } else {
                        PM_TINY_LOG_E("%s env_vars wrong type, must be a scalar or sequence",
                                      prog_cfg.name.c_str());
                    }
                }
                auto start_timeout_node = prop["start_timeout"];
                if (start_timeout_node) {
                    prog_cfg.start_timeout = start_timeout_node.as<int>();
                }
                auto failure_action_node = prop["failure_action"];
                if (failure_action_node) {
                    auto action = failure_action_node.as<std::string>();
                    try {
                        prog_cfg.failure_action = str_to_failure_action(action);
                    } catch (const std::exception &ex) {
                        PM_TINY_LOG_E("failure_action `%s` invalid,must be `skip` or `restart` ",
                                      action.c_str());
                    }
                }
                auto daemon_node = prop["daemon"];
                if (daemon_node) {
                    prog_cfg.daemon = daemon_node.as<bool>();
                }
                auto heartbeat_timeout_node = prop["heartbeat_timeout"];
                if (heartbeat_timeout_node) {
                    prog_cfg.heartbeat_timeout = heartbeat_timeout_node.as<int>();
                }
                auto oom_score_adj_node=prop["oom_score_adj"];
                if (oom_score_adj_node) {
                    prog_cfg.oom_score_adj = oom_score_adj_node.as<int>();
                }
                prog_cfg.envs = load_app_environ(prog_cfg.name, app_environ_dir);
                prog_cfgs.push_back(prog_cfg);
            }
        }
        return prog_cfgs;
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

    std::unique_ptr<prog_cfg_graph_t> check_prog_cfg(const std::vector<prog_cfg_t> &prog_cfgs) {
        std::unordered_map<std::string, int> name2idx;
        for (int i = 0; i < static_cast<int>(prog_cfgs.size()); i++) {
            auto &name = prog_cfgs[i].name;
            if (name2idx.count(name) > 0) {
                logger->error("name %s already exists ignore", name.c_str());
                return nullptr;
            }
            name2idx[name] = i;
        }
        int vertex_count = static_cast<int>(name2idx.size() + 1);
        auto graph = std::make_unique<prog_cfg_graph_t>(vertex_count);
        for (int i = 0; i < static_cast<int>(prog_cfgs.size()); i++) {
            const auto &cfg = prog_cfgs[i];
            if (cfg.depends_on.empty()) {
                graph->add_edge(0, i + 1);
            } else {
                for (auto &sc: cfg.depends_on) {
                    if (name2idx.count(sc) > 0) {
                        graph->add_edge(name2idx[sc] + 1, i + 1);
                    } else {
                        logger->error("depends_on: %s not found", sc.c_str());
                        return nullptr;
                    }
                }
            }
            graph->vertex(i + 1) = const_cast<prog_cfg_t *>(&cfg);
        }
        graph->vertex(0) = nullptr;
        auto no_cycle = graph->topological_sort();
        if (!no_cycle) {
            return nullptr;
        }
        return graph;
    }
}