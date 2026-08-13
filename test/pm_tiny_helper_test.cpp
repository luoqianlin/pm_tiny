#include "core/pm_tiny_helper.h"
#include "core/pm_tiny.h"

#include <algorithm>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

[[noreturn]] void fail(const char *message) {
    std::cerr << "pm_tiny_helper_test failure: " << message
              << " (errno=" << errno << ": " << std::strerror(errno) << ")\n";
    std::abort();
}

void expect(bool condition, const char *message) {
    if (!condition) {
        fail(message);
    }
}

struct EnvEntry {
    std::string name;
    bool existed{};
    std::string value;
};

class EnvScope {
public:
    explicit EnvScope(const std::vector<std::string> &names) {
        for (const auto &name: names) {
            const char *current = ::getenv(name.c_str());
            entries_.push_back({name, current != nullptr, current ? current : ""});
        }
    }

    ~EnvScope() {
        restore();
    }

    void set(const char *name, const std::string &value) {
        ensure_entry(name);
        ::setenv(name, value.c_str(), 1);
    }

    void unset(const char *name) {
        ensure_entry(name);
        ::unsetenv(name);
    }

private:
    std::vector<EnvEntry> entries_;

    void ensure_entry(const char *name) {
        auto it = std::find_if(entries_.begin(), entries_.end(),
                               [name](const EnvEntry &entry) {
                                   return entry.name == name;
                               });
        if (it == entries_.end()) {
            const char *current = ::getenv(name);
            entries_.push_back({name, current != nullptr, current ? current : ""});
        }
    }

    void restore() {
        for (auto &entry: entries_) {
            if (entry.existed) {
                ::setenv(entry.name.c_str(), entry.value.c_str(), 1);
            } else {
                ::unsetenv(entry.name.c_str());
            }
        }
    }
};

std::string make_temp_dir(const std::string &prefix) {
    std::string templ = prefix + "XXXXXX";
    std::vector<char> buffer(templ.begin(), templ.end());
    buffer.push_back('\0');
    char *dir_path = ::mkdtemp(buffer.data());
    expect(dir_path != nullptr, "mkdtemp failed");
    return std::string(dir_path);
}

void make_dir(const std::string &path) {
    int rc = ::mkdir(path.c_str(), 0700);
    if (rc != 0 && errno != EEXIST) {
        fail("mkdir failed");
    }
}

std::string write_config_file(const std::string &dir,
                              const std::string &home,
                              const std::string &log,
                              const std::string &prog_cfg,
                              const std::string &app_log,
                              const std::string &app_env,
                              const std::string &sock,
                              bool uds,
                              const std::string &tree_mode = "auto",
                              const std::string &cgroup_root = "") {
    const std::string cfg_path = dir + "/pm_tiny.yaml";
    std::ofstream ofs(cfg_path);
    ofs << "pm_tiny_home_dir: " << home << "\n";
    ofs << "pm_tiny_log_file: " << log << "\n";
    ofs << "pm_tiny_prog_cfg_file: " << prog_cfg << "\n";
    ofs << "pm_tiny_app_log_dir: " << app_log << "\n";
    ofs << "pm_tiny_app_environ_dir: " << app_env << "\n";
    ofs << "pm_tiny_sock_file: " << sock << "\n";
    ofs << "pm_tiny_uds_abstract_namespace: " << (uds ? "true" : "false") << "\n";
    ofs << "pm_tiny_process_tree_mode: " << tree_mode << "\n";
    ofs << "pm_tiny_cgroup_root: " << cgroup_root << "\n";
    ofs.close();
    expect(ofs.good(), "failed to write configuration file");
    return cfg_path;
}

void test_env_overrides_config() {
    const std::vector<std::string> env_names = {
            PM_TINY_HOME,
            PM_TINY_LOG_FILE,
            PM_TINY_PROG_CFG_FILE,
            PM_TINY_APP_LOG_DIR,
            PM_TINY_APP_ENVIRON_DIR,
            PM_TINY_SOCK_FILE,
            PM_TINY_UDS_ABSTRACT_NAMESPACE,
            "PM_TINY_PROCESS_TREE_MODE", "PM_TINY_CGROUP_ROOT"
    };
    EnvScope env_guard(env_names);
    for (const auto &name: env_names) {
        env_guard.unset(name.c_str());
    }

    const std::string base_dir = make_temp_dir("/tmp/pm_tiny_helper_env_");
    const std::string env_home_trim = base_dir + "/home";
    make_dir(env_home_trim);
    const std::string env_home = env_home_trim + "/";
    const std::string env_log = env_home_trim + "/pm_tiny.log";
    const std::string env_prog_cfg = env_home_trim + "/prog.yaml";
    const std::string env_app_log_trim = env_home_trim + "/logs";
    make_dir(env_app_log_trim);
    const std::string env_app_log = env_app_log_trim + "/";
    const std::string env_app_env_trim = env_home_trim + "/environ";
    make_dir(env_app_env_trim);
    const std::string env_app_env = env_app_env_trim + "/";
    const std::string env_sock = "pm_tinyd.custom";

    const std::string cfg_home = base_dir + "/cfg_home";
    make_dir(cfg_home);
    const std::string cfg_log = cfg_home + "/log";
    const std::string cfg_prog = cfg_home + "/prog.yaml";
    const std::string cfg_app_log = cfg_home + "/logs/";
    const std::string cfg_app_env = cfg_home + "/environ/";
    const std::string cfg_sock = cfg_home + "/pm.sock";
    const std::string cfg_path = write_config_file(base_dir, cfg_home + "/", cfg_log,
                                                   cfg_prog, cfg_app_log,
                                                   cfg_app_env, cfg_sock, false);

    env_guard.set(PM_TINY_HOME, env_home);
    env_guard.set(PM_TINY_LOG_FILE, env_log);
    env_guard.set(PM_TINY_PROG_CFG_FILE, env_prog_cfg);
    env_guard.set(PM_TINY_APP_LOG_DIR, env_app_log);
    env_guard.set(PM_TINY_APP_ENVIRON_DIR, env_app_env);
    env_guard.set(PM_TINY_SOCK_FILE, env_sock);
    env_guard.set(PM_TINY_UDS_ABSTRACT_NAMESPACE, "Y");
    env_guard.set("PM_TINY_PROCESS_TREE_MODE", "process_group");
    env_guard.set("PM_TINY_CGROUP_ROOT", "/tmp/env-cgroup");

    auto cfg = pm_tiny::get_pm_tiny_config(cfg_path);
    expect(static_cast<bool>(cfg), "expected configuration object");
    expect(cfg->pm_tiny_home_dir == env_home_trim, "env home override failed");
    expect(cfg->pm_tiny_log_file == env_log, "env log override failed");
    expect(cfg->pm_tiny_prog_cfg_file == env_prog_cfg, "env prog cfg override failed");
    expect(cfg->pm_tiny_app_log_dir == env_app_log_trim, "env app log override failed");
    expect(cfg->pm_tiny_app_environ_dir == env_app_env_trim, "env environ override failed");
    expect(cfg->pm_tiny_sock_file == env_sock, "env sock override failed");
    expect(cfg->uds_abstract_namespace, "env uds flag not applied");
    expect(cfg->process_tree_mode == "process_group", "env process-tree mode override failed");
    expect(cfg->cgroup_root == "/tmp/env-cgroup", "env cgroup root override failed");
    expect(cfg->pm_tiny_lock_file == env_home_trim + "/pm_tiny.pid",
           "lock file not derived from env");

    const char *updated_home = ::getenv(PM_TINY_HOME);
    const char *updated_sock = ::getenv(PM_TINY_SOCK_FILE);
    expect(updated_home && std::string(updated_home) == env_home_trim,
           "PM_TINY_HOME env not exported");
    expect(updated_sock && std::string(updated_sock) == env_sock,
           "PM_TINY_SOCK_FILE env not exported");
}

void test_config_file_fallback() {
    const std::vector<std::string> env_names = {
            PM_TINY_HOME,
            PM_TINY_LOG_FILE,
            PM_TINY_PROG_CFG_FILE,
            PM_TINY_APP_LOG_DIR,
            PM_TINY_APP_ENVIRON_DIR,
            PM_TINY_SOCK_FILE,
            PM_TINY_UDS_ABSTRACT_NAMESPACE,
            "PM_TINY_PROCESS_TREE_MODE", "PM_TINY_CGROUP_ROOT"
    };
    EnvScope env_guard(env_names);
    for (const auto &name: env_names) {
        env_guard.unset(name.c_str());
    }

    const std::string base_dir = make_temp_dir("/tmp/pm_tiny_helper_cfg_");
    const std::string cfg_home_trim = base_dir + "/cfg_home";
    make_dir(cfg_home_trim);
    const std::string cfg_home = cfg_home_trim + "/";
    const std::string cfg_log = cfg_home_trim + "/pm.log";
    const std::string cfg_prog = cfg_home_trim + "/prog.yaml";
    const std::string cfg_app_log_trim = cfg_home_trim + "/logs";
    make_dir(cfg_app_log_trim);
    const std::string cfg_app_log = cfg_app_log_trim + "/";
    const std::string cfg_app_env_trim = cfg_home_trim + "/environ";
    make_dir(cfg_app_env_trim);
    const std::string cfg_app_env = cfg_app_env_trim + "/";
    const std::string cfg_sock = cfg_home_trim + "/pm.sock";
    const std::string cfg_path = write_config_file(base_dir, cfg_home, cfg_log, cfg_prog,
                                                   cfg_app_log, cfg_app_env,
                                                   cfg_sock, true, "cgroup", "/sys/fs/cgroup");

    auto cfg = pm_tiny::get_pm_tiny_config(cfg_path);
    expect(static_cast<bool>(cfg), "expected configuration object (fallback)");
    expect(cfg->pm_tiny_home_dir == cfg_home_trim, "cfg home mismatch");
    expect(cfg->pm_tiny_log_file == cfg_log, "cfg log mismatch");
    expect(cfg->pm_tiny_prog_cfg_file == cfg_prog, "cfg prog mismatch");
    expect(cfg->pm_tiny_app_log_dir == cfg_app_log_trim, "cfg app log mismatch");
    expect(cfg->pm_tiny_app_environ_dir == cfg_app_env_trim, "cfg env mismatch");
    expect(cfg->pm_tiny_sock_file == cfg_sock, "cfg sock mismatch");
    expect(cfg->uds_abstract_namespace, "cfg uds flag mismatch");
    expect(cfg->process_tree_mode == "cgroup", "cfg process-tree mode mismatch");
    expect(cfg->cgroup_root == "/sys/fs/cgroup", "cfg cgroup root mismatch");

    const char *home = ::getenv(PM_TINY_HOME);
    const char *prog_cfg = ::getenv(PM_TINY_PROG_CFG_FILE);
    const char *uds_flag = ::getenv(PM_TINY_UDS_ABSTRACT_NAMESPACE);
    expect(home && std::string(home) == cfg_home_trim, "PM_TINY_HOME env mismatch");
    expect(prog_cfg && std::string(prog_cfg) == cfg_prog, "PM_TINY_PROG_CFG_FILE env mismatch");
    expect(uds_flag && std::string(uds_flag) == "1", "PM_TINY_UDS_ABSTRACT_NAMESPACE env mismatch");
}

} // namespace

int main() {
    test_env_overrides_config();
    test_config_file_fallback();
    return 0;
}
