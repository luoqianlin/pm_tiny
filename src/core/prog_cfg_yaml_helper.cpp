#include "prog_cfg_yaml_helper.h"

#include <yaml-cpp/yaml.h>

#include <sstream>
#include <utility>

namespace pm_tiny {
namespace {

std::vector<std::string> parse_string_sequence(const YAML::Node &node,
                                               ProgCfgParseResult &result,
                                               const std::string &field_name,
                                               const std::string &entry_name) {
    std::vector<std::string> values;
    if (!node) {
        return values;
    }
    if (node.IsScalar()) {
        try {
            values.push_back(node.as<std::string>());
        } catch (const YAML::Exception &ex) {
            std::ostringstream oss;
            oss << "Program `" << entry_name << "` has invalid scalar in field `" << field_name
                << "`: " << ex.what();
            result.warnings.push_back(oss.str());
        }
        return values;
    }
    if (node.IsSequence()) {
        for (auto &&element : node) {
            try {
                values.push_back(element.as<std::string>());
            } catch (const YAML::Exception &ex) {
                std::ostringstream oss;
                oss << "Program `" << entry_name << "` has invalid element in field `" << field_name
                    << "`: " << ex.what();
                result.warnings.push_back(oss.str());
            }
        }
        return values;
    }
    std::ostringstream oss;
    oss << "Program `" << entry_name << "` field `" << field_name
        << "` must be a scalar or sequence";
    result.warnings.push_back(oss.str());
    return values;
}

bool read_required_string(const YAML::Node &node,
                          const char *key,
                          std::string &target,
                          ProgCfgParseResult &result) {
    auto value_node = node[key];
    if (!value_node) {
        std::ostringstream oss;
        oss << "Config entry missing required field `" << key << "`";
        result.error = oss.str();
        return false;
    }
    try {
        target = value_node.as<std::string>();
        return true;
    } catch (const YAML::Exception &ex) {
        std::ostringstream oss;
        oss << "Config field `" << key << "` must be a string: " << ex.what();
        result.error = oss.str();
        return false;
    }
}

bool read_optional_string(const YAML::Node &node,
                          const char *key,
                          std::string &target,
                          ProgCfgParseResult &result,
                          const std::string &entry_name) {
    auto value_node = node[key];
    if (!value_node) {
        return true;
    }
    try {
        target = value_node.as<std::string>();
        return true;
    } catch (const YAML::Exception &ex) {
        std::ostringstream oss;
        oss << "Program `" << entry_name << "` field `" << key << "` invalid: " << ex.what();
        result.error = oss.str();
        return false;
    }
}

bool read_optional_int(const YAML::Node &node,
                       const char *key,
                       int &target,
                       ProgCfgParseResult &result,
                       const std::string &entry_name) {
    auto value_node = node[key];
    if (!value_node) {
        return true;
    }
    try {
        target = value_node.as<int>();
        return true;
    } catch (const YAML::Exception &ex) {
        std::ostringstream oss;
        oss << "Program `" << entry_name << "` field `" << key << "` invalid: " << ex.what();
        result.error = oss.str();
        return false;
    }
}

bool validate_range(int value,
                    int minimum,
                    int maximum,
                    const char *key,
                    ProgCfgParseResult &result,
                    const std::string &entry_name) {
    if (value >= minimum && value <= maximum) return true;
    std::ostringstream oss;
    oss << "Program `" << entry_name << "` field `" << key << "` must be between "
        << minimum << " and " << maximum;
    result.error = oss.str();
    return false;
}

} // namespace

ProgCfgParseResult parse_prog_cfg_yaml_node(const YAML::Node &node, prog_cfg_t &out_cfg) {
    ProgCfgParseResult result;
    prog_cfg_t parsed;

    if (!read_required_string(node, "name", parsed.name, result)) {
        return result;
    }
    if (!read_required_string(node, "cwd", parsed.cwd, result)) {
        return result;
    }
    if (!read_required_string(node, "command", parsed.command, result)) {
        return result;
    }

    if (auto kill_timeout = node["kill_timeout_s"]) {
        if (!read_optional_int(node, "kill_timeout_s", parsed.kill_timeout_s, result, parsed.name)) {
            return result;
        }
    } else if (node["kill_timeout"]) {
        if (!read_optional_int(node, "kill_timeout", parsed.kill_timeout_s, result, parsed.name)) {
            return result;
        }
    }

    std::string run_as_value;
    if (!read_optional_string(node, "user", run_as_value, result, parsed.name)) {
        return result;
    }
    if (run_as_value.empty()) {
        if (!read_optional_string(node, "run_as", run_as_value, result, parsed.name)) {
            return result;
        }
    }
    if (run_as_value.empty()) {
        if (!read_optional_string(node, "run_as_user", run_as_value, result, parsed.name)) {
            return result;
        }
    }
    parsed.run_as = run_as_value;

    parsed.depends_on = parse_string_sequence(node["depends_on"], result, "depends_on", parsed.name);
    parsed.env_vars = parse_string_sequence(node["env_vars"], result, "env_vars", parsed.name);

    if (!read_optional_int(node, "start_timeout", parsed.start_timeout, result, parsed.name)) {
        return result;
    }

    auto failure_action_node = node["failure_action"];
    if (failure_action_node) {
        try {
            auto action_str = failure_action_node.as<std::string>();
            try {
                parsed.failure_action = str_to_failure_action(action_str);
            } catch (const std::exception &ex) {
                std::ostringstream oss;
                oss << "Program `" << parsed.name << "` field `failure_action` invalid: " << ex.what();
                result.warnings.push_back(oss.str());
            }
        } catch (const YAML::Exception &ex) {
            std::ostringstream oss;
            oss << "Program `" << parsed.name << "` field `failure_action` invalid: " << ex.what();
            result.error = oss.str();
            return result;
        }
    }

    if (!read_optional_int(node, "heartbeat_timeout", parsed.heartbeat_timeout, result, parsed.name)) {
        return result;
    }

    if (!read_optional_int(node, "oom_score_adj", parsed.oom_score_adj, result, parsed.name)) {
        return result;
    }

    if (!read_optional_int(node, "restart_delay_ms", parsed.restart_delay_ms, result, parsed.name) ||
        !read_optional_int(node, "restart_max_delay_ms", parsed.restart_max_delay_ms, result, parsed.name) ||
        !read_optional_int(node, "restart_window_ms", parsed.restart_window_ms, result, parsed.name) ||
        !read_optional_int(node, "restart_max_attempts", parsed.restart_max_attempts, result, parsed.name) ||
        !read_optional_int(node, "restart_reset_after_ms", parsed.restart_reset_after_ms, result, parsed.name)) {
        return result;
    }
    if (!validate_range(parsed.restart_delay_ms, 0, restart_duration_max_ms,
                        "restart_delay_ms", result, parsed.name) ||
        !validate_range(parsed.restart_max_delay_ms, 0, restart_duration_max_ms,
                        "restart_max_delay_ms", result, parsed.name) ||
        !validate_range(parsed.restart_window_ms, 1, restart_duration_max_ms,
                        "restart_window_ms", result, parsed.name) ||
        !validate_range(parsed.restart_max_attempts, 0, restart_attempts_max,
                        "restart_max_attempts", result, parsed.name) ||
        !validate_range(parsed.restart_reset_after_ms, 0, restart_duration_max_ms,
                        "restart_reset_after_ms", result, parsed.name)) {
        return result;
    }
    if (parsed.restart_max_delay_ms < parsed.restart_delay_ms) {
        std::ostringstream oss;
        oss << "Program `" << parsed.name
            << "` field `restart_max_delay_ms` must be greater than or equal to `restart_delay_ms`";
        result.error = oss.str();
        return result;
    }

    if (auto daemon_node = node["daemon"]) {
        try {
            parsed.daemon = daemon_node.as<bool>();
        } catch (const YAML::Exception &ex) {
            std::ostringstream oss;
            oss << "Program `" << parsed.name << "` field `daemon` invalid: " << ex.what();
            result.error = oss.str();
            return result;
        }
    }

    // Optional: pty flag, default to true if absent
    if (auto pty_node = node["pty"]) {
        try {
            parsed.pty = pty_node.as<bool>();
        } catch (const YAML::Exception &ex) {
            std::ostringstream oss;
            oss << "Program `" << parsed.name << "` field `pty` invalid: " << ex.what();
            result.error = oss.str();
            return result;
        }
    }

    if (!result.warnings.empty()) {
        result.error = result.warnings.front();
        result.warnings.clear();
        return result;
    }

    out_cfg = std::move(parsed);
    result.success = true;
    return result;
}

ProgCfgDocumentParseResult parse_prog_cfg_yaml_document(const YAML::Node &root) {
    ProgCfgDocumentParseResult result;
    if (!root) {
        result.error = "Config is empty or invalid";
        return result;
    }
    if (!root.IsSequence()) {
        result.error = "Config must contain a top-level sequence";
        return result;
    }
    result.programs.reserve(root.size());
    for (std::size_t index = 0; index < root.size(); ++index) {
        prog_cfg_t cfg;
        auto entry = parse_prog_cfg_yaml_node(root[index], cfg);
        result.warnings.insert(result.warnings.end(), entry.warnings.begin(), entry.warnings.end());
        if (!entry.success) {
            std::ostringstream error;
            error << "Config entry " << index << " invalid: " << entry.error;
            result.error = error.str();
            result.programs.clear();
            return result;
        }
        result.programs.push_back(std::move(cfg));
    }
    result.success = true;
    return result;
}

YAML::Node serialize_prog_cfg_yaml_node(const prog_cfg_t &cfg,
                                        const ProgCfgSerializeOptions &options) {
    YAML::Node node;
    node["name"] = cfg.name;
    node["cwd"] = cfg.cwd;
    node["command"] = cfg.command;
    node["env_vars"] = cfg.env_vars;
    node["kill_timeout_s"] = cfg.kill_timeout_s;
    if (options.include_run_as) node["user"] = cfg.run_as;
    node["depends_on"] = cfg.depends_on;
    node["start_timeout"] = cfg.start_timeout;
    node["failure_action"] = failure_action_to_str(cfg.failure_action);
    node["daemon"] = cfg.daemon;
    node["heartbeat_timeout"] = cfg.heartbeat_timeout;
    if (options.include_oom_score_adj) node["oom_score_adj"] = cfg.oom_score_adj;
    if (options.include_pty) node["pty"] = cfg.pty;
    node["restart_delay_ms"] = cfg.restart_delay_ms;
    node["restart_max_delay_ms"] = cfg.restart_max_delay_ms;
    node["restart_window_ms"] = cfg.restart_window_ms;
    node["restart_max_attempts"] = cfg.restart_max_attempts;
    node["restart_reset_after_ms"] = cfg.restart_reset_after_ms;
    return node;
}

} // namespace pm_tiny
