#include "launch_environment.h"

#include <unordered_map>

namespace pm_tiny {
namespace {

std::string env_key(const std::string &entry) {
    const auto separator = entry.find('=');
    return separator == std::string::npos ? std::string() : entry.substr(0, separator);
}

bool is_identity_key(const std::string &key) {
    return key == "HOME" || key == "USER" || key == "LOGNAME" || key == "SHELL";
}

bool remove_inherited_key(const std::string &key) {
    return key == "PATH" || is_identity_key(key) || key.compare(0, 5, "SUDO_") == 0 ||
           key.compare(0, 3, "LD_") == 0;
}

} // namespace

bool executable_has_path(const std::string &executable) {
    return executable.find('/') != std::string::npos;
}

std::vector<std::string> compose_launch_environment(
        const std::vector<std::string> &inherited,
        const std::vector<std::string> &explicit_values,
        const passwd_t *target_user,
        bool sanitize_for_user_switch) {
    std::vector<std::string> result;
    std::unordered_map<std::string, std::size_t> indices;
    const auto apply = [&](const std::string &entry) {
        const auto key = env_key(entry);
        if (key.empty() || key.compare(0, 8, "PM_TINY_") == 0) return;
        const auto found = indices.find(key);
        if (found == indices.end()) {
            indices[key] = result.size();
            result.push_back(entry);
        } else {
            result[found->second] = entry;
        }
    };

    for (const auto &entry : inherited) {
        const auto key = env_key(entry);
        if (sanitize_for_user_switch && remove_inherited_key(key)) continue;
        apply(entry);
    }
    if (sanitize_for_user_switch && target_user != nullptr) {
        if (!target_user->pw_dir.empty()) apply("HOME=" + target_user->pw_dir);
        if (!target_user->pw_name.empty()) {
            apply("USER=" + target_user->pw_name);
            apply("LOGNAME=" + target_user->pw_name);
        }
        if (!target_user->pw_shell.empty()) apply("SHELL=" + target_user->pw_shell);
    }
    for (const auto &entry : explicit_values) apply(entry);
    return result;
}

} // namespace pm_tiny
