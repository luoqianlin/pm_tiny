#include "daemon/prog_cfg.h"
#include "core/daemon_log.h"
#include "core/prog_cfg_yaml_helper.h"
#include "core/dependency_graph.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

[[noreturn]] void fail(const char *message) {
    std::fprintf(stderr, "prog_cfg_test failure: %s\n", message);
    std::abort();
}

void expect(bool condition, const char *message) {
    if (!condition) {
        fail(message);
    }
}

pm_tiny::prog_cfg_t make_cfg(const std::string &name,
                             std::vector<std::string> depends = {}) {
    pm_tiny::prog_cfg_t cfg{};
    cfg.name = name;
    cfg.cwd = "/tmp";
    cfg.executable = "/bin/true";
    cfg.kill_timeout_s = 3;
    cfg.run_as = "root";
    cfg.depends_on = std::move(depends);
    cfg.start_timeout = 0;
    cfg.failure_action = pm_tiny::failure_action_t::SKIP;
    cfg.daemon = true;
    cfg.heartbeat_timeout = -1;
    cfg.oom_score_adj = 0;
    return cfg;
}

void test_valid_dependency_graph() {
    std::vector<pm_tiny::prog_cfg_t> cfgs;
    cfgs.push_back(make_cfg("alpha"));
    cfgs.push_back(make_cfg("beta", {"alpha"}));
    cfgs.push_back(make_cfg("gamma", {"beta"}));

    std::vector<pm_tiny::dependency_node_config> nodes;
    for (const auto &cfg : cfgs) nodes.push_back({cfg.name, cfg.depends_on});
    pm_tiny::dependency_graph graph;
    pm_tiny::dependency_error error;
    expect(pm_tiny::dependency_graph::build(nodes, graph, error), "expected dependency graph to be created");
    expect(graph.size() == cfgs.size(), "unexpected vertex count");
    expect(graph.dependencies(graph.find("beta"))[0] == graph.find("alpha"), "beta should depend on alpha");
    expect(graph.dependencies(graph.find("gamma"))[0] == graph.find("beta"), "gamma should depend on beta");
}

void test_duplicate_names_rejected() {
    std::vector<pm_tiny::prog_cfg_t> cfgs;
    cfgs.push_back(make_cfg("duplicate"));
    cfgs.push_back(make_cfg("duplicate"));
    std::vector<pm_tiny::dependency_node_config> nodes;
    for (const auto &cfg : cfgs) nodes.push_back({cfg.name, cfg.depends_on});
    pm_tiny::dependency_graph graph;
    pm_tiny::dependency_error error;
    expect(!pm_tiny::dependency_graph::build(nodes, graph, error), "duplicate names should fail validation");
}

void test_missing_dependency_rejected() {
    std::vector<pm_tiny::prog_cfg_t> cfgs;
    cfgs.push_back(make_cfg("lonely", {"missing"}));
    pm_tiny::dependency_graph graph;
    pm_tiny::dependency_error error;
    expect(!pm_tiny::dependency_graph::build({{"lonely", {"missing"}}}, graph, error),
           "unknown dependency should fail validation");
}

void test_cycle_detection() {
    std::vector<pm_tiny::prog_cfg_t> cfgs;
    cfgs.push_back(make_cfg("one", {"two"}));
    cfgs.push_back(make_cfg("two", {"one"}));
    pm_tiny::dependency_graph graph;
    pm_tiny::dependency_error error;
    expect(!pm_tiny::dependency_graph::build({{"one", {"two"}}, {"two", {"one"}}}, graph, error),
           "cyclic dependencies should fail validation");
}

void test_save_load_round_trip() {
    const std::string base = "/tmp/pm_tiny_prog_cfg_test_" + std::to_string(static_cast<long long>(getpid()));
    const std::string cfg_path = base + ".yaml";
    const std::string env_dir = base + ".env";
    mkdir(env_dir.c_str(), 0700);
    auto first = make_cfg("roundtrip");
    first.cwd = "/opt/pm tiny";
    first.executable = "/usr/bin/demo";
    first.args = {"--flag", "value with space", ""};
    first.kill_timeout_s = 9;
    first.run_as = "operator";
    first.envs = {"BASE=one", "SPECIAL=two", "EMPTY=", "SPACES=  kept  ",
                  "MULTI=a=b=c", std::string("LINES=first\nsecond")};
    first.env_vars = {"EXTRA=yes"};
    first.start_timeout = 7;
    first.failure_action = pm_tiny::failure_action_t::RESTART;
    first.daemon = false;
    first.heartbeat_timeout = 11;
    first.oom_score_adj = 42;
    first.pty = false;
    first.log_mode = pm_tiny::log_mode_t::combined;
    first.log_dir = "/var/log/apps";
    first.log_file_name = "roundtrip.log";
    first.log_max_size_kb = 8192;
    first.log_archive_count = 7;
    first.restart_delay_ms = 125;
    first.restart_max_delay_ms = 4000;
    first.restart_window_ms = 45000;
    first.restart_max_attempts = 6;
    first.restart_reset_after_ms = 90000;
    expect(pm_tiny::save_prog_cfg({first}, cfg_path, env_dir) == 0, "roundtrip save failed");
    auto loaded = pm_tiny::load_prog_cfg(cfg_path, env_dir);
    expect(loaded.success && loaded.programs.size() == 1, "roundtrip entry count mismatch");
    const auto &second = loaded.programs.front();
    expect(second.name == first.name && second.cwd == first.cwd && second.executable == first.executable &&
           second.args == first.args,
           "roundtrip basic fields mismatch");
    expect(second.kill_timeout_s == first.kill_timeout_s && second.run_as == first.run_as,
           "roundtrip runtime fields mismatch");
    expect(second.depends_on == first.depends_on && second.env_vars == first.env_vars,
           "roundtrip dependency/env fields mismatch");
    expect(second.start_timeout == first.start_timeout && second.failure_action == first.failure_action,
           "roundtrip failure fields mismatch");
    expect(second.daemon == first.daemon && second.heartbeat_timeout == first.heartbeat_timeout &&
           second.pty == first.pty, "roundtrip flags mismatch");
    expect(second.log_mode == first.log_mode && second.log_dir == first.log_dir &&
           second.log_file_name == first.log_file_name &&
           second.log_max_size_kb == first.log_max_size_kb &&
           second.log_archive_count == first.log_archive_count,
           "roundtrip log fields mismatch");
    expect(second.restart_delay_ms == first.restart_delay_ms &&
           second.restart_max_delay_ms == first.restart_max_delay_ms &&
           second.restart_window_ms == first.restart_window_ms &&
           second.restart_max_attempts == first.restart_max_attempts &&
           second.restart_reset_after_ms == first.restart_reset_after_ms,
           "roundtrip restart policy mismatch");
    auto envs = pm_tiny::load_app_environ("roundtrip", env_dir);
    expect(envs == first.envs, "roundtrip environment file mismatch");
    expect(pm_tiny::save_prog_cfg({}, cfg_path, env_dir) == 0, "empty transactional save failed");
    const auto empty = pm_tiny::load_prog_cfg(cfg_path, env_dir);
    expect(empty.success && empty.programs.empty(), "empty config did not persist");
    expect(access((env_dir + "/roundtrip.yaml").c_str(), F_OK) != 0, "stale environment file survived save");
    unlink(cfg_path.c_str());
    rmdir(env_dir.c_str());
}

void test_missing_and_contentless_configs_are_empty() {
    const std::string base = "/tmp/pm_tiny_prog_cfg_load_" +
                             std::to_string(static_cast<long long>(getpid()));
    const auto missing = pm_tiny::load_prog_cfg(base + ".yaml", base + ".env");
    expect(missing.success && missing.programs.empty(), "missing config must load as empty");

    const std::vector<std::string> contentless = {
        "", " \t\r\n", "# comment only\n  # another comment\r\n",
        std::string("\xef\xbb\xbf") + "  # comment after BOM\n"
    };
    for (const auto &content : contentless) {
        {
            std::ofstream empty(base + ".yaml", std::ios::binary | std::ios::trunc);
            empty << content;
        }
        const auto loaded = pm_tiny::load_prog_cfg(base + ".yaml", base + ".env");
        expect(loaded.success && loaded.programs.empty(),
               "contentless config must load as empty");
    }

    const std::vector<std::string> invalid_documents = {
        "null\n", "~\n", "{}\n", "scalar\n"
    };
    for (const auto &content : invalid_documents) {
        {
            std::ofstream invalid(base + ".yaml", std::ios::trunc);
            invalid << content;
        }
        const auto loaded = pm_tiny::load_prog_cfg(base + ".yaml", base + ".env");
        expect(!loaded.success && !loaded.error.empty(),
               "non-sequence YAML document must fail explicitly");
    }

    {
        std::ofstream invalid(base + ".yaml");
        invalid << "- name: broken\n  executable: [\n";
    }
    const auto malformed = pm_tiny::load_prog_cfg(base + ".yaml", base + ".env");
    expect(!malformed.success && !malformed.error.empty(), "malformed config must fail explicitly");
    unlink((base + ".yaml").c_str());
}

void test_yaml_parser_regressions() {
    const std::string base = "/tmp/pm_tiny_prog_cfg_yaml_parser_" +
                             std::to_string(static_cast<long long>(getpid()));
    const std::string cfg_path = base + ".yaml";

    {
        std::ofstream config(cfg_path, std::ios::binary | std::ios::trunc);
        config << "- name: cr-only\r"
                  "  cwd: /tmp\r"
                  "  executable: /bin/true\r";
    }
    const auto cr_only = pm_tiny::load_prog_cfg(cfg_path, base + ".env");
    expect(cr_only.success && cr_only.programs.size() == 1 &&
           cr_only.programs.front().name == "cr-only",
           "CR-only YAML config should load");

    {
        std::ofstream config(cfg_path, std::ios::binary | std::ios::trunc);
        config << "- name: \"broken\" \"suffix\"\n"
                  "  cwd: /tmp\n"
                  "  executable: /bin/true\n";
    }
    const auto adjacent_scalars = pm_tiny::load_prog_cfg(cfg_path, base + ".env");
    expect(!adjacent_scalars.success && !adjacent_scalars.error.empty(),
           "adjacent quoted YAML scalars must fail");
    unlink(cfg_path.c_str());
}

void test_environment_stage_failure_preserves_config() {
    const std::string base = "/tmp/pm_tiny_prog_cfg_fail_" +
                             std::to_string(static_cast<long long>(getpid()));
    const std::string cfg_path = base + ".yaml";
    const std::string env_dir = base + ".env";
    mkdir(env_dir.c_str(), 0700);
    {
        std::ofstream cfg(cfg_path);
        cfg << "original-config\n";
        std::ofstream env(env_dir + "/original.yaml");
        env << "OLD=1\n";
    }
    auto replacement = make_cfg("replacement");
    replacement.envs = {"NEW=1"};
    expect(pm_tiny::save_prog_cfg({replacement}, cfg_path, base + "/missing/env") != 0,
           "invalid environment stage unexpectedly succeeded");
    std::ifstream cfg(cfg_path);
    std::stringstream cfg_content;
    cfg_content << cfg.rdbuf();
    expect(cfg_content.str() == "original-config\n", "failed save changed existing config");
    std::ifstream env(env_dir + "/original.yaml");
    std::stringstream env_content;
    env_content << env.rdbuf();
    expect(env_content.str() == "OLD=1\n", "failed save changed existing environment");
    unlink(cfg_path.c_str());
    unlink((env_dir + "/original.yaml").c_str());
    rmdir(env_dir.c_str());
}

void test_interrupted_save_recovery() {
    const std::string base = "/tmp/pm_tiny_prog_cfg_recovery_" +
                             std::to_string(static_cast<long long>(getpid()));
    const std::string cfg_path = base + ".yaml";
    const std::string env_dir = base + ".env";
    auto original = make_cfg("original");
    original.envs = {"VALUE=old"};
    auto replacement = make_cfg("replacement");
    replacement.envs = {"VALUE=new"};

    const char *steps[] = {"prepared", "old_moved", "new_installed"};
    for (const char *step : steps) {
        expect(pm_tiny::save_prog_cfg({original}, cfg_path, env_dir) == 0,
               "failed to prepare recovery fixture");
        setenv("PM_TINY_TEST_FAIL_SAVE_STEP", step, 1);
        expect(pm_tiny::save_prog_cfg({replacement}, cfg_path, env_dir) != 0,
               "fault-injected save unexpectedly succeeded");
        unsetenv("PM_TINY_TEST_FAIL_SAVE_STEP");

        const auto recovered = pm_tiny::load_prog_cfg(cfg_path, env_dir);
        expect(recovered.success && recovered.programs.size() == 1,
               "interrupted save did not recover");
        const std::string expected = std::string(step) == "new_installed" ?
                                     "replacement" : "original";
        expect(recovered.programs.front().name == expected,
               "interrupted save recovered the wrong transaction version");
        const auto envs = pm_tiny::load_app_environ(expected, env_dir);
        expect(envs == (expected == "replacement" ? replacement.envs : original.envs),
               "interrupted save recovered the wrong environment version");
        expect(access((cfg_path + ".save-journal").c_str(), F_OK) != 0,
               "recovered save journal was not removed");
    }
    expect(pm_tiny::save_prog_cfg({}, cfg_path, env_dir) == 0,
           "failed to clean recovery fixture");
    unlink(cfg_path.c_str());
    rmdir(env_dir.c_str());
}

void test_invalid_entry_rejects_entire_document() {
    YAML::Node root = YAML::Load(R"(
- name: valid
  cwd: /tmp
  executable: /bin/true
- name: invalid
  cwd: /tmp
)");
    const auto result = pm_tiny::parse_prog_cfg_yaml_document(root);
    expect(!result.success, "invalid entry should reject the document");
    expect(result.programs.empty(), "invalid document should not return a partial config");
}

void test_invalid_failure_action_rejects_document() {
    const auto result = pm_tiny::parse_prog_cfg_yaml_document(YAML::Load(R"(
- name: invalid
  cwd: /tmp
  executable: /bin/true
  failure_action: explode
)"));
    expect(!result.success, "invalid failure_action should reject the document");
    expect(result.error.find("failure_action") != std::string::npos,
           "invalid failure_action should identify the field");
}

void test_invalid_sequence_element_rejects_document() {
    const auto result = pm_tiny::parse_prog_cfg_yaml_document(YAML::Load(R"(
- name: invalid
  cwd: /tmp
  executable: /bin/true
  depends_on:
    - valid
    - {bad: value}
)"));
    expect(!result.success, "invalid sequence element should reject the document");
    expect(result.error.find("depends_on") != std::string::npos,
           "invalid sequence element should identify the field");
}

void test_platform_serialization_can_omit_unsupported_fields() {
    auto cfg = make_cfg("platform");
    pm_tiny::ProgCfgSerializeOptions options;
    options.include_run_as = false;
    options.include_oom_score_adj = false;
    options.include_pty = false;
    const auto node = pm_tiny::serialize_prog_cfg_yaml_node(cfg, options);
    expect(!node["user"] && !node["oom_score_adj"] && !node["pty"],
           "platform serializer should omit unsupported fields");
    expect(node["name"].as<std::string>() == cfg.name &&
           node["executable"].as<std::string>() == cfg.executable && node["args"].IsSequence(),
           "platform serializer should retain common fields");
}

void expect_invalid_restart_config(const char *yaml, const char *expected_error) {
    const auto result = pm_tiny::parse_prog_cfg_yaml_document(YAML::Load(yaml));
    expect(!result.success, "invalid restart config should fail");
    expect(result.error.find(expected_error) != std::string::npos,
           "invalid restart config should report the offending field");
}

void test_restart_policy_validation() {
    expect_invalid_restart_config(R"(
- name: invalid
  cwd: /tmp
  executable: /bin/true
  restart_delay_ms: -1
)", "restart_delay_ms");
    expect_invalid_restart_config(R"(
- name: invalid
  cwd: /tmp
  executable: /bin/true
  restart_window_ms: 0
)", "restart_window_ms");
    expect_invalid_restart_config(R"(
- name: invalid
  cwd: /tmp
  executable: /bin/true
  restart_max_attempts: 100001
)", "restart_max_attempts");
    expect_invalid_restart_config(R"(
- name: invalid
  cwd: /tmp
  executable: /bin/true
  restart_delay_ms: 2000
  restart_max_delay_ms: 1000
)", "restart_max_delay_ms");
}

void test_log_config_validation() {
    const auto valid_pty = pm_tiny::parse_prog_cfg_yaml_document(YAML::Load(R"(
- name: pty-app
  cwd: /tmp
  executable: /bin/true
  pty: true
)"));
    expect(valid_pty.success && valid_pty.programs[0].log_mode == pm_tiny::log_mode_t::combined,
           "PTY should default to combined logging");

    const char *invalid_documents[] = {
R"(- name: old
  cwd: /tmp
  executable: /bin/true
  log_file_count: 3
)",
R"(- name: split-pty
  cwd: /tmp
  executable: /bin/true
  pty: true
  log_mode: split
)",
R"(- name: size
  cwd: /tmp
  executable: /bin/true
  log_max_size_kb: 0
)",
R"(- name: archives
  cwd: /tmp
  executable: /bin/true
  log_archive_count: 101
)"};
    for (const auto *document : invalid_documents) {
        expect(!pm_tiny::parse_prog_cfg_yaml_document(YAML::Load(document)).success,
               "invalid log configuration should fail");
    }
}

void test_removed_environment_formats_and_reserved_names() {
    const char *invalid_documents[] = {
R"(- name: inherited
  cwd: /tmp
  executable: /bin/true
  inherited_env: [VALUE=old]
)",
R"(- name: reserved
  cwd: /tmp
  executable: /bin/true
  env_vars: [PM_TINY_HOME=/tmp/override]
)"};
    for (const auto *document : invalid_documents) {
        expect(!pm_tiny::parse_prog_cfg_yaml_document(YAML::Load(document)).success,
               "removed or reserved environment field should fail");
    }
}

} // namespace

int main() {
    pm_tiny::reset_daemon_log();
    test_valid_dependency_graph();
    test_duplicate_names_rejected();
    test_missing_dependency_rejected();
    test_cycle_detection();
    test_save_load_round_trip();
    test_missing_and_contentless_configs_are_empty();
    test_yaml_parser_regressions();
    test_environment_stage_failure_preserves_config();
    test_interrupted_save_recovery();
    test_invalid_entry_rejects_entire_document();
    test_invalid_failure_action_rejects_document();
    test_invalid_sequence_element_rejects_document();
    test_platform_serialization_can_omit_unsupported_fields();
    test_restart_policy_validation();
    test_log_config_validation();
    test_removed_environment_formats_and_reserved_names();
    return 0;
}
