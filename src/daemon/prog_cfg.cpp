#include "prog_cfg.h"
#include "daemon_log.h"
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
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <ctime>

namespace {
bool path_exists(const std::string &path) { return access(path.c_str(), F_OK) == 0; }

std::string parent_directory(const std::string &path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? "." : (slash == 0 ? "/" : path.substr(0, slash));
}

bool sync_file(const std::string &path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

bool sync_directory(const std::string &path) {
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

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

struct save_transaction_t {
    std::string state;
    std::string suffix;
    std::string cfg_path;
    std::string cfg_temp;
    std::string cfg_backup;
    std::string env_path;
    std::string env_stage;
    std::string env_backup;
    bool had_cfg = false;
    bool had_env = false;
};

bool sync_transaction_directories(const save_transaction_t &transaction) {
    const bool cfg_ok = sync_directory(parent_directory(transaction.cfg_path));
    if (parent_directory(transaction.env_path) == parent_directory(transaction.cfg_path)) return cfg_ok;
    return sync_directory(parent_directory(transaction.env_path)) && cfg_ok;
}

bool write_save_journal(const std::string &journal, const save_transaction_t &transaction) {
    YAML::Node root;
    root["schema"] = 1;
    root["state"] = transaction.state;
    root["suffix"] = transaction.suffix;
    root["cfg_path"] = transaction.cfg_path;
    root["cfg_temp"] = transaction.cfg_temp;
    root["cfg_backup"] = transaction.cfg_backup;
    root["env_path"] = transaction.env_path;
    root["env_stage"] = transaction.env_stage;
    root["env_backup"] = transaction.env_backup;
    root["had_cfg"] = transaction.had_cfg;
    root["had_env"] = transaction.had_env;

    const std::string journal_temp = journal + ".tmp";
    {
        std::ofstream out(journal_temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << root << "\n";
        out.flush();
        if (!out.good()) return false;
    }
    if (!sync_file(journal_temp) || rename(journal_temp.c_str(), journal.c_str()) != 0) {
        unlink(journal_temp.c_str());
        return false;
    }
    return sync_directory(parent_directory(journal));
}

bool load_save_journal(const std::string &journal, save_transaction_t &transaction,
                       std::string &error) {
    try {
        const YAML::Node root = YAML::LoadFile(journal);
        if (!root.IsMap() || !root["schema"] || root["schema"].as<int>() != 1) {
            error = "unsupported save journal schema";
            return false;
        }
        transaction.state = root["state"].as<std::string>();
        transaction.suffix = root["suffix"].as<std::string>();
        transaction.cfg_path = root["cfg_path"].as<std::string>();
        transaction.cfg_temp = root["cfg_temp"].as<std::string>();
        transaction.cfg_backup = root["cfg_backup"].as<std::string>();
        transaction.env_path = root["env_path"].as<std::string>();
        transaction.env_stage = root["env_stage"].as<std::string>();
        transaction.env_backup = root["env_backup"].as<std::string>();
        transaction.had_cfg = root["had_cfg"].as<bool>();
        transaction.had_env = root["had_env"].as<bool>();
    } catch (const YAML::Exception &ex) {
        error = ex.what();
        return false;
    }
    return true;
}

bool valid_save_transaction(const save_transaction_t &transaction,
                            const std::string &cfg_path,
                            const std::string &env_path) {
    return !transaction.suffix.empty() && transaction.suffix.find(".txn.") == 0 &&
           transaction.cfg_path == cfg_path && transaction.env_path == env_path &&
           transaction.cfg_temp == cfg_path + transaction.suffix &&
           transaction.cfg_backup == cfg_path + ".bak" + transaction.suffix &&
           transaction.env_stage == env_path + transaction.suffix &&
           transaction.env_backup == env_path + ".bak" + transaction.suffix &&
           (transaction.state == "prepared" || transaction.state == "old_moved" ||
            transaction.state == "new_installed");
}

bool restore_backup(const std::string &backup, const std::string &path, bool had_original,
                    bool directory) {
    if (path_exists(backup)) {
        if (directory) remove_flat_directory(path); else unlink(path.c_str());
        return rename(backup.c_str(), path.c_str()) == 0;
    }
    if (!had_original) {
        if (directory) remove_flat_directory(path); else unlink(path.c_str());
    }
    return !had_original || path_exists(path);
}

#ifdef PM_TINY_TESTING
bool fail_save_at(const char *step) {
    const char *configured = std::getenv("PM_TINY_TEST_FAIL_SAVE_STEP");
    return configured && std::string(configured) == step;
}

void apply_test_save_delay() {
    const char *configured = std::getenv("PM_TINY_TEST_SAVE_DELAY_MS");
    if (!configured) return;
    char *end = nullptr;
    const long delay_ms = std::strtol(configured, &end, 10);
    if (end != configured && *end == '\0' && delay_ms > 0 && delay_ms <= 10000) {
        usleep(static_cast<useconds_t>(delay_ms) * 1000);
    }
}
#else
bool fail_save_at(const char *) { return false; }
void apply_test_save_delay() {}
#endif
}

namespace pm_tiny {
    bool recover_prog_cfg_save(const std::string &cfg_path,
                               const std::string &app_environ_dir,
                               std::string &error) {
        const std::string journal = cfg_path + ".save-journal";
        if (!path_exists(journal)) return true;

        save_transaction_t transaction;
        if (!load_save_journal(journal, transaction, error) ||
            !valid_save_transaction(transaction, cfg_path, app_environ_dir)) {
            if (error.empty()) error = "save journal paths or state are invalid";
            error = "Cannot recover " + journal + ": " + error;
            return false;
        }

        bool ok = true;
        if (transaction.state == "new_installed") {
            if (!path_exists(transaction.cfg_path) || !path_exists(transaction.env_path)) {
                error = "Cannot recover committed save transaction: installed files are missing";
                return false;
            }
            unlink(transaction.cfg_backup.c_str());
            remove_flat_directory(transaction.env_backup);
        } else {
            ok = restore_backup(transaction.cfg_backup, transaction.cfg_path,
                                transaction.had_cfg, false) && ok;
            ok = restore_backup(transaction.env_backup, transaction.env_path,
                                transaction.had_env, true) && ok;
        }
        unlink(transaction.cfg_temp.c_str());
        remove_flat_directory(transaction.env_stage);
        if (!ok || !sync_transaction_directories(transaction)) {
            error = "Failed to restore interrupted save transaction " + journal;
            return false;
        }
        if (unlink(journal.c_str()) != 0 && errno != ENOENT) {
            error = "Failed to remove recovered save journal " + journal;
            return false;
        }
        if (!sync_directory(parent_directory(journal))) {
            error = "Failed to sync recovered save journal directory";
            return false;
        }
        return true;
    }

    std::ostream &operator<<(std::ostream &os, const prog_cfg_t &prog_cfg) {
        os << "=== " << prog_cfg.name << " ===" << std::endl;
        os << "executable:" << prog_cfg.executable << std::endl;
        os << "args:[" << mgr::utils::join(prog_cfg.args, ",") << "]" << std::endl;
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
        apply_test_save_delay();
//        std::stringstream ss;
        std::vector<std::tuple<std::string, std::string>> f_envs;
        YAML::Node rootNode;
        for (const auto &cfg: cfgs) {
//            ss << cfg.name << ":" << cfg.cwd << ":" << cfg.command
//               << ":" << cfg.kill_timeout_s << ":" << cfg.run_as << "\n";
            YAML::Node prop = serialize_prog_cfg_yaml_node(cfg);
            rootNode.push_back(prop);
            YAML::Node env_root;
            env_root["schema"] = 1;
            env_root["environment"] = cfg.envs;
            YAML::Emitter env_out;
            env_out << env_root;
            std::stringstream env_ss;
            env_ss << env_out.c_str() << "\n";
            f_envs.emplace_back(cfg.name, env_ss.str());
        }
        std::string recovery_error;
        if (!recover_prog_cfg_save(cfg_path, app_environ_dir, recovery_error)) {
            PM_TINY_DLOG_ERROR("%s", recovery_error.c_str());
            return -1;
        }
        const auto suffix = ".txn." + std::to_string(static_cast<long long>(getpid())) + "." +
                            std::to_string(static_cast<long long>(std::time(nullptr)));
        const std::string tmp_path = cfg_path + suffix;
        const std::string cfg_backup = cfg_path + ".bak" + suffix;
        const std::string env_stage = app_environ_dir + suffix;
        const std::string env_backup = app_environ_dir + ".bak" + suffix;
        remove_flat_directory(env_stage);
        remove_flat_directory(env_backup);
        unlink(cfg_backup.c_str());
        if (mkdir(env_stage.c_str(), 0700) != 0) {
            PM_TINY_DLOG_ERROR("cannot create temporary environ dir %s", env_stage.c_str());
            return -1;
        }
        for (auto &env: f_envs) {
            const auto filename = env_stage + "/" + std::get<0>(env) + ".yaml";
            std::fstream env_fs(filename, std::ios::out | std::ios::trunc);
            if (!env_fs) {
                remove_flat_directory(env_stage);
                PM_TINY_DLOG_ERROR("%s write fail", std::get<0>(env).c_str());
                return -1;
            }
            env_fs << std::get<1>(env);
            env_fs.flush();
            if (!env_fs.good()) {
                env_fs.close();
                remove_flat_directory(env_stage);
                return -1;
            }
            env_fs.close();
            if (!sync_file(filename)) {
                remove_flat_directory(env_stage);
                return -1;
            }
        }
        if (!sync_directory(env_stage)) {
            remove_flat_directory(env_stage);
            return -1;
        }
        std::fstream cfg_file(tmp_path, std::ios::out | std::ios::trunc);
        if (!cfg_file) {
            remove_flat_directory(env_stage);
            PM_TINY_DLOG_ERROR("cannot open temporary cfg %s", tmp_path.c_str());
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
            PM_TINY_DLOG_ERROR("cannot write cfg %s", tmp_path.c_str());
            return -1;
        }
        cfg_file.close();
        if (!sync_file(tmp_path)) {
            unlink(tmp_path.c_str());
            remove_flat_directory(env_stage);
            return -1;
        }
        const bool had_cfg = path_exists(cfg_path);
        const bool had_env = path_exists(app_environ_dir);
        const std::string journal = cfg_path + ".save-journal";
        save_transaction_t transaction{"prepared", suffix, cfg_path, tmp_path, cfg_backup,
                                       app_environ_dir, env_stage, env_backup, had_cfg, had_env};
        if (!write_save_journal(journal, transaction)) return -1;
        if (fail_save_at("prepared")) return -1;
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
        if (!sync_transaction_directories(transaction)) return -1;
        transaction.state = "old_moved";
        if (!write_save_journal(journal, transaction)) return -1;
        if (fail_save_at("old_moved")) return -1;
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
        if (!sync_transaction_directories(transaction)) return -1;
        transaction.state = "new_installed";
        if (!write_save_journal(journal, transaction)) return -1;
        if (fail_save_at("new_installed")) return -1;
        unlink(cfg_backup.c_str());
        remove_flat_directory(env_backup);
        if (!sync_transaction_directories(transaction)) return -1;
        unlink(journal.c_str());
        sync_directory(parent_directory(cfg_path));
        return 0;
    }

    std::vector<std::string> load_app_environ(const std::string &name,
                                              const std::string &app_environ_dir) {
        std::vector<std::string> envs;
        const auto legacy_path = app_environ_dir + "/" + name;
        const auto path = legacy_path + ".yaml";
        if (path_exists(legacy_path)) {
            throw std::runtime_error("Legacy environment sidecar `" + legacy_path +
                                     "` is unsupported; migrate it to the 3.1 YAML sidecar format");
        }
        std::fstream efs(path);
        if (!efs) {
            PM_TINY_DLOG_DEBUG("%s environ not exists", name.c_str());
            for (char **env = environ; *env != nullptr; env++) {
                envs.emplace_back(*env);
            }
            return envs;
        }

        YAML::Node root = YAML::Load(efs);
        if (!root.IsMap() || !root["schema"] || root["schema"].as<int>() != 1 ||
            !root["environment"] || !root["environment"].IsSequence())
            throw std::runtime_error("Invalid environment sidecar: " + path);
        for (const auto &entry : root["environment"]) envs.push_back(entry.as<std::string>());
        return envs;
    }

    prog_cfg_load_result_t load_prog_cfg_yaml(const std::string &cfg_path,
                                               const std::string &app_environ_dir) {
        prog_cfg_load_result_t result;
        std::ifstream input(cfg_path, std::ios::binary);
        if (!input) {
            if (errno == ENOENT) {
                result.success = true;
                return result;
            }
            result.error = "Failed to open cfg " + cfg_path + ": " + std::strerror(errno);
            PM_TINY_DLOG_ERROR("%s", result.error.c_str());
            return result;
        }
        std::ostringstream content_stream;
        content_stream << input.rdbuf();
        if (input.bad()) {
            result.error = "Failed to read cfg " + cfg_path;
            PM_TINY_DLOG_ERROR("%s", result.error.c_str());
            return result;
        }
        const auto content = content_stream.str();
        if (is_effectively_empty_prog_cfg_yaml(content)) {
            result.success = true;
            return result;
        }

        YAML::Node progNodes;
        try {
            progNodes = YAML::Load(content);
        } catch (const YAML::Exception &ex) {
            result.error = "Failed to load cfg " + cfg_path + ": " + ex.what();
            PM_TINY_DLOG_ERROR("%s", result.error.c_str());
            return result;
        }

        auto document = parse_prog_cfg_yaml_document(progNodes);
        for (const auto &warning : document.warnings) PM_TINY_DLOG_ERROR("%s", warning.c_str());
        if (!document.success) {
            PM_TINY_DLOG_ERROR("cfg file %s invalid: %s", cfg_path.c_str(), document.error.c_str());
            result.error = "cfg file " + cfg_path + " invalid: " + document.error;
            return result;
        }
        std::string order_error;
        if (!validate_and_order_prog_cfgs(document.programs, order_error)) {
            result.error = order_error;
            return result;
        }
        try {
            for (auto &cfg : document.programs) cfg.envs = load_app_environ(cfg.name, app_environ_dir);
        } catch (const std::exception &ex) {
            result.error = ex.what();
            return result;
        }
        result.success = true;
        result.programs = std::move(document.programs);
        return result;
    }


    prog_cfg_load_result_t load_prog_cfg(const std::string &cfg_path,
                                         const std::string &app_environ_dir) {
        std::string name, ext;
        std::tie(name, ext) = mgr::utils::splitext(cfg_path);
        if (ext != ".yaml" && ext != ".yml") {
            PM_TINY_DLOG_ERROR("3.0 config must be YAML: %s", cfg_path.c_str());
            return {false, {}, "3.0 config must be YAML: " + cfg_path};
        }
        std::string recovery_error;
        if (!recover_prog_cfg_save(cfg_path, app_environ_dir, recovery_error)) {
            PM_TINY_DLOG_ERROR("%s", recovery_error.c_str());
            return {false, {}, recovery_error};
        }
        if (access(cfg_path.c_str(), F_OK) != 0) {
            if (errno == ENOENT) {
                PM_TINY_DLOG_INFO("program config not found; starting empty: %s", cfg_path.c_str());
                return {true, {}, {}};
            }
            const std::string error = "cannot access cfg " + cfg_path + ": " + std::strerror(errno);
            PM_TINY_DLOG_ERROR("%s", error.c_str());
            return {false, {}, error};
        }
        return load_prog_cfg_yaml(cfg_path, app_environ_dir);
    }

}
