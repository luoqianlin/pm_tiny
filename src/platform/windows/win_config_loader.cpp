#include "win_config_loader.h"

#include "core/prog_cfg_yaml_helper.h"
#include "core/prog_cfg_order.h"
#include "daemon_log.h"
#include "win_utils.h"
#include "windows_program_persistence.h"

#include <yaml-cpp/yaml.h>

#include <windows.h>

#include <algorithm>
#include <cwchar>
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

void emit_warnings(const std::vector<std::string> &warnings) {
    for (const auto &w : warnings) {
        daemon_log_message(daemon_log_level_t::warn, w);
    }
}

bool path_exists(const std::string &path) {
    const DWORD attributes = GetFileAttributesW(utf8_to_wide(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES;
}

bool read_file(const std::string &path, std::string &content, std::string &error) {
    HANDLE file = CreateFileW(utf8_to_wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "Cannot open `" + path + "`: " + std::to_string(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 64LL * 1024 * 1024) {
        error = "Cannot read size of `" + path + "`";
        CloseHandle(file);
        return false;
    }
    content.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD total = 0;
    while (total < content.size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(content.size() - total, 1024 * 1024));
        if (!ReadFile(file, &content[total], chunk, &read, nullptr)) {
            error = "Cannot read `" + path + "`: " + std::to_string(GetLastError());
            CloseHandle(file);
            return false;
        }
        if (read == 0) break;
        total += read;
    }
    content.resize(total);
    CloseHandle(file);
    return true;
}

std::vector<std::string> current_environment() {
    std::vector<std::string> result;
    LPWCH block = GetEnvironmentStringsW();
    if (block == nullptr) return result;
    for (LPCWCH item = block; *item != L'\0'; item += wcslen(item) + 1) {
        if (*item == L'=') continue;
        result.push_back(wide_to_utf8(item));
    }
    FreeEnvironmentStringsW(block);
    return result;
}

bool load_environment_sidecar(const std::string &name, const std::string &directory,
                              std::vector<std::string> &environment, std::string &error) {
    const std::string base = directory + "\\" + name;
    if (path_exists(base)) {
        error = "Legacy environment sidecar `" + base +
                "` is unsupported; migrate it to the YAML sidecar format";
        return false;
    }
    const std::string path = base + ".yaml";
    if (!path_exists(path)) {
        environment = current_environment();
        return true;
    }
    std::string content;
    if (!read_file(path, content, error)) return false;
    try {
        const YAML::Node root = YAML::Load(content);
        if (!root.IsMap() || !root["schema"] || root["schema"].as<int>() != 1 ||
            !root["environment"] || !root["environment"].IsSequence()) {
            error = "Invalid environment sidecar `" + path + "`";
            return false;
        }
        environment.clear();
        for (const auto &entry : root["environment"])
            environment.push_back(entry.as<std::string>());
    } catch (const YAML::Exception &ex) {
        error = "Invalid environment sidecar `" + path + "`: " + ex.what();
        return false;
    }
    return true;
}

} // namespace

ConfigLoadResult load_program_configs(const std::string &program_config_path,
                                      const std::string &app_environ_dir) {
    ConfigLoadResult result;
    if (!recover_program_config_save(program_config_path, app_environ_dir, result.error_message))
        return result;
    const DWORD attributes = GetFileAttributesW(utf8_to_wide(program_config_path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return result;
    }
    std::string content;
    if (!read_file(program_config_path, content, result.error_message)) return result;
    if (is_effectively_empty_prog_cfg_yaml(content)) return result;
    YAML::Node programs;
    try { programs = YAML::Load(content); }
    catch (const YAML::Exception &ex) {
        result.error_message = "Failed to load program config `" + program_config_path + "`: " + ex.what();
        return result;
    }

    auto document = parse_prog_cfg_yaml_document(programs);
    emit_warnings(document.warnings);
    if (!document.success) {
        result.error_message = "Program config `" + program_config_path +
            "` invalid: " + document.error;
        return result;
    }

    for (std::size_t index = 0; index < document.programs.size(); ++index) {
        const auto &node = programs[index];
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
        if (node["failure_action"] && base_cfg.failure_action == failure_action_t::REBOOT) {
            result.error_message = "Program `" + base_cfg.name +
                "` field `failure_action: reboot` is unsupported on Windows; supported values: skip, restart";
            result.programs.clear();
            return result;
        }

        ProgramConfig cfg = make_program_config(base_cfg);
        if (!load_environment_sidecar(cfg.name, app_environ_dir, cfg.envs, result.error_message)) {
            result.programs.clear();
            return result;
        }

        result.programs.push_back(std::move(cfg));
    }

    if (result.programs.empty() && programs.size() == 0) {
        return result;
    }
    if (!validate_and_order_prog_cfgs(result.programs, result.error_message)) {
        result.programs.clear();
    }
    return result;
}

} // namespace win
} // namespace pm_tiny
