#include "win_config_loader.h"

#include "core/prog_cfg_yaml_helper.h"
#include "core/prog_cfg_order.h"

#include <yaml-cpp/yaml.h>

#include <iostream>
#include <sstream>

namespace pm_tiny {
namespace win {

namespace {

ProgramConfig make_program_config(const prog_cfg_t &base) {
    ProgramConfig cfg;
    static_cast<prog_cfg_t &>(cfg) = base;
    return cfg;
}

void apply_windows_overrides(const YAML::Node &node,
                             ProgramConfig &cfg,
                             std::vector<std::string> &warnings) {
    auto append_warning = [&](const std::string &message) {
        warnings.push_back(message);
    };

    auto read_optional_int = [&](const char *key, int &target) {
        auto value = node[key];
        if (!value) {
            return;
        }
        try {
            target = value.as<int>();
        } catch (const YAML::Exception &ex) {
            std::ostringstream oss;
            oss << "Program `" << cfg.name << "` field `" << key << "` invalid: " << ex.what();
            append_warning(oss.str());
        }
    };

    auto read_optional_string = [&](const char *key, std::string &target) {
        auto value = node[key];
        if (!value) {
            return;
        }
        try {
            target = value.as<std::string>();
        } catch (const YAML::Exception &ex) {
            std::ostringstream oss;
            oss << "Program `" << cfg.name << "` field `" << key << "` invalid: " << ex.what();
            append_warning(oss.str());
        }
    };

    read_optional_int("log_max_size_kb", cfg.log_max_size_kb);
    if (!node["log_max_size_kb"] && node["log_size_kb"]) {
        read_optional_int("log_size_kb", cfg.log_max_size_kb);
    }
    read_optional_int("log_file_count", cfg.log_file_count);
    if (!node["log_file_count"] && node["log_files"]) {
        read_optional_int("log_files", cfg.log_file_count);
    }

    read_optional_string("log_dir", cfg.log_dir);
    read_optional_string("log_file_name", cfg.log_file_name);
    if (cfg.log_file_name.empty() && node["log_file"]) {
        read_optional_string("log_file", cfg.log_file_name);
    }
}

void emit_warnings(const std::vector<std::string> &warnings) {
    for (const auto &w : warnings) {
        std::cerr << "[WARN] " << w << std::endl;
    }
}

} // namespace

ConfigLoadResult load_program_configs(const std::string &path) {
    ConfigLoadResult result;
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception &ex) {
        result.error_message = "Failed to load config `" + path + "`: " + ex.what();
        return result;
    }

    auto document = parse_prog_cfg_yaml_document(root);
    emit_warnings(document.warnings);
    if (!document.success) {
        result.error_message = "Config file `" + path + "` invalid: " + document.error;
        return result;
    }

    for (std::size_t index = 0; index < document.programs.size(); ++index) {
        const auto &node = root[index];
        auto base_cfg = std::move(document.programs[index]);
        if (!base_cfg.run_as.empty()) {
            result.error_message = "Program `" + base_cfg.name + "` field `user` is unsupported on Windows";
            result.programs.clear();
            return result;
        }
        if (base_cfg.oom_score_adj != 0) {
            result.error_message = "Program `" + base_cfg.name + "` field `oom_score_adj` is unsupported on Windows";
            result.programs.clear();
            return result;
        }
        if (node["pty"] && base_cfg.pty) {
            result.error_message = "Program `" + base_cfg.name + "` field `pty: true` is unsupported on Windows";
            result.programs.clear();
            return result;
        }
        if (node["failure_action"] && base_cfg.failure_action != failure_action_t::SKIP) {
            result.error_message = "Program `" + base_cfg.name + "` field `failure_action` is unsupported on Windows";
            result.programs.clear();
            return result;
        }

        ProgramConfig cfg = make_program_config(base_cfg);
        std::vector<std::string> platform_warnings;
        apply_windows_overrides(node, cfg, platform_warnings);
        emit_warnings(platform_warnings);

        if (cfg.log_file_name.empty()) {
            cfg.log_file_name = cfg.name.empty() ? "pm_tiny.log" : cfg.name + ".log";
        }
        if (cfg.log_max_size_kb <= 0) {
            cfg.log_max_size_kb = 4096;
        }
        if (cfg.log_file_count <= 0) {
            cfg.log_file_count = 3;
        }

        result.programs.push_back(std::move(cfg));
    }

    if (result.programs.empty() && root.size() == 0) {
        return result;
    }
    if (!validate_and_order_prog_cfgs(result.programs, result.error_message)) {
        result.programs.clear();
    }
    return result;
}

} // namespace win
} // namespace pm_tiny
