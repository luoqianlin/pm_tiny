#include "daemon_config.h"

#include "pm_tiny.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace pm_tiny {
namespace {

bool valid_log_level(const std::string &value) {
    return value == "debug" || value == "info" || value == "warn" ||
           value == "error" || value == "fatal";
}

bool parse_long(const std::string &value, long minimum, long maximum, long &parsed) {
    if (value.empty()) return false;
    char *end = nullptr;
    errno = 0;
    parsed = std::strtol(value.c_str(), &end, 10);
    return errno == 0 && end != value.c_str() && *end == '\0' &&
           parsed >= minimum && parsed <= maximum;
}

bool parse_bool(const std::string &value, bool &parsed) {
    if (value == "1" || value == "Y" || value == "y" || value == "true" || value == "yes") {
        parsed = true;
        return true;
    }
    if (value == "0" || value == "N" || value == "n" || value == "false" || value == "no") {
        parsed = false;
        return true;
    }
    return false;
}

char path_separator(daemon_platform platform) {
    return platform == daemon_platform::windows ? '\\' : '/';
}

bool is_absolute_path(const std::string &path, daemon_platform platform) {
    if (path.empty()) return false;
    if (platform == daemon_platform::windows) {
        return (path.size() >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') ||
                                    (path[0] >= 'a' && path[0] <= 'z')) &&
                path[1] == ':' && (path[2] == '\\' || path[2] == '/')) ||
               (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') ||
               path[0] == '\\' || path[0] == '/';
    }
    return path[0] == '/';
}

std::string parent_directory(const std::string &path, daemon_platform platform) {
    const auto slash = platform == daemon_platform::windows
        ? path.find_last_of("\\/") : path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return path.substr(0, 1);
    return path.substr(0, slash);
}

std::string join_path(const std::string &base, const std::string &name,
                      daemon_platform platform) {
    if (base.empty()) return name;
    if (name.empty()) return base;
    if (base.back() == '/' || base.back() == '\\') return base + name;
    return base + path_separator(platform) + name;
}

void trim_trailing_separators(std::string &path, daemon_platform platform) {
    while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) {
        if (platform == daemon_platform::windows && path.size() == 3 && path[1] == ':') break;
        path.pop_back();
    }
}

void resolve_relative(const std::string &config_path, std::string &path,
                      daemon_platform platform) {
    if (path.empty() || is_absolute_path(path, platform)) return;
    path = join_path(parent_directory(config_path, platform), path, platform);
}

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string &value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("invalid UTF-8");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), &result[0], size) <= 0)
        throw std::runtime_error("invalid UTF-8");
    return result;
}

std::string wide_to_utf8(const std::wstring &value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("invalid UTF-16");
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), &result[0], size, nullptr, nullptr) <= 0)
        throw std::runtime_error("invalid UTF-16");
    return result;
}
#endif

std::string default_home(daemon_platform platform) {
    if (platform == daemon_platform::windows) {
        const std::string profile = daemon_environment("USERPROFILE");
        if (!profile.empty()) return join_path(profile, ".pm_tiny", platform);
        const std::string drive = daemon_environment("HOMEDRIVE");
        const std::string path = daemon_environment("HOMEPATH");
        if (!drive.empty() && !path.empty()) return join_path(drive + path, ".pm_tiny", platform);
        return ".pm_tiny";
    }
#ifndef _WIN32
    const passwd *pw = getpwuid(getuid());
    if (pw != nullptr && pw->pw_dir != nullptr) return join_path(pw->pw_dir, ".pm_tiny", platform);
#endif
    const std::string home = daemon_environment("HOME");
    return home.empty() ? ".pm_tiny" : join_path(home, ".pm_tiny", platform);
}

std::string implicit_config_path(const daemon_cli_options &options,
                                 daemon_platform platform) {
    if (platform == daemon_platform::posix) return PM_TINY_DEFAULT_CFG_FILE;
    std::string home = options.home_explicit ? options.home_dir : daemon_environment(PM_TINY_HOME);
    if (home.empty()) home = default_home(platform);
    return join_path(home, "pm_tiny.yaml", platform);
}

bool file_exists(const std::string &path) {
#ifdef _WIN32
    try {
        const DWORD attributes = GetFileAttributesW(utf8_to_wide(path).c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    } catch (...) {
        return false;
    }
#else
    return access(path.c_str(), F_OK | R_OK) == 0;
#endif
}

bool load_yaml_file(const std::string &path, YAML::Node &root, std::string &error) {
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    try { file = CreateFileW(utf8_to_wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr); }
    catch (const std::exception &ex) {
        error = "Invalid daemon config path `" + path + "`: " + ex.what();
        return false;
    }
    if (file == INVALID_HANDLE_VALUE) {
        error = "Cannot open daemon config `" + path + "`: " + std::to_string(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 16LL * 1024 * 1024) {
        error = "Cannot read daemon config size `" + path + "`";
        CloseHandle(file);
        return false;
    }
    std::string content(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD total = 0;
    while (total < content.size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(content.size() - total, 1024 * 1024));
        if (!ReadFile(file, &content[total], chunk, &read, nullptr)) {
            error = "Cannot read daemon config `" + path + "`: " + std::to_string(GetLastError());
            CloseHandle(file);
            return false;
        }
        if (read == 0) break;
        total += read;
    }
    CloseHandle(file);
    content.resize(total);
    try { root = YAML::Load(content); }
    catch (const YAML::Exception &ex) {
        error = "Failed to load daemon config `" + path + "`: " + ex.what();
        return false;
    }
    return true;
#else
    try { root = YAML::LoadFile(path); }
    catch (const YAML::Exception &ex) {
        error = "Failed to load daemon config `" + path + "`: " + ex.what();
        return false;
    }
    return true;
#endif
}

template <typename T>
bool read_value(const YAML::Node &root, const char *key, T &target, std::string &error) {
    if (!root[key]) return true;
    try {
        target = root[key].as<T>();
        return true;
    } catch (const YAML::Exception &ex) {
        error = std::string("Invalid daemon field `") + key + "`: " + ex.what();
        return false;
    }
}

bool validate_keys(const YAML::Node &root, daemon_platform platform, std::string &error) {
    const std::set<std::string> common = {
        "pm_tiny_home_dir", "pm_tiny_log_file", "pm_tiny_prog_cfg_file",
        "pm_tiny_app_log_dir", "pm_tiny_app_environ_dir", "pm_tiny_log_level",
        "pm_tiny_log_max_size_kb", "pm_tiny_log_archive_count"
    };
    const std::set<std::string> posix = {
        "pm_tiny_sock_file", "pm_tiny_uds_abstract_namespace", "pm_tiny_allowed_uids",
        "pm_tiny_allowed_gids", "pm_tiny_process_tree_mode", "pm_tiny_cgroup_root"
    };
    const std::set<std::string> windows = {"pm_tiny_pipe_name", "pm_tiny_pipe_sddl"};
    for (const auto &entry : root) {
        std::string key;
        try { key = entry.first.as<std::string>(); }
        catch (const YAML::Exception &ex) {
            error = std::string("Invalid daemon configuration key: ") + ex.what();
            return false;
        }
        if (common.count(key) != 0) continue;
        if (platform == daemon_platform::posix && posix.count(key) != 0) continue;
        if (platform == daemon_platform::windows && windows.count(key) != 0) continue;
        if (posix.count(key) != 0 || windows.count(key) != 0) {
            error = "Daemon field `" + key + "` is unsupported on this platform";
            return false;
        }
        if (key.compare(0, 8, "pm_tiny_") == 0) {
            error = "Unknown daemon field `" + key + "`";
            return false;
        }
    }
    return true;
}

bool load_file_config(const std::string &path, daemon_platform platform,
                      daemon_config &config, std::string &error) {
    YAML::Node root;
    if (!load_yaml_file(path, root, error)) return false;
    if (!root.IsMap()) {
        error = "Daemon config `" + path + "` must be a mapping";
        return false;
    }
    if (!validate_keys(root, platform, error)) return false;
    if (!read_value(root, "pm_tiny_home_dir", config.home_dir, error) ||
        !read_value(root, "pm_tiny_log_file", config.log_file, error) ||
        !read_value(root, "pm_tiny_prog_cfg_file", config.program_config_file, error) ||
        !read_value(root, "pm_tiny_app_log_dir", config.app_log_dir, error) ||
        !read_value(root, "pm_tiny_app_environ_dir", config.app_environ_dir, error) ||
        !read_value(root, "pm_tiny_log_level", config.log_level, error) ||
        !read_value(root, "pm_tiny_log_max_size_kb", config.log_max_size_kb, error) ||
        !read_value(root, "pm_tiny_log_archive_count", config.log_archive_count, error)) return false;
    if (platform == daemon_platform::posix) {
        if (!read_value(root, "pm_tiny_sock_file", config.socket_file, error) ||
            !read_value(root, "pm_tiny_uds_abstract_namespace", config.uds_abstract_namespace, error) ||
            !read_value(root, "pm_tiny_process_tree_mode", config.process_tree_mode, error) ||
            !read_value(root, "pm_tiny_cgroup_root", config.cgroup_root, error) ||
            !read_value(root, "pm_tiny_allowed_uids", config.allowed_uids, error) ||
            !read_value(root, "pm_tiny_allowed_gids", config.allowed_gids, error)) return false;
    } else {
        if (!read_value(root, "pm_tiny_pipe_name", config.pipe_name, error) ||
            !read_value(root, "pm_tiny_pipe_sddl", config.pipe_sddl, error)) return false;
    }
    const std::pair<const char *, const char *> common_sources[] = {
        {"pm_tiny_home_dir", "home_dir"}, {"pm_tiny_log_file", "log_file"},
        {"pm_tiny_prog_cfg_file", "program_config_file"},
        {"pm_tiny_app_log_dir", "app_log_dir"},
        {"pm_tiny_app_environ_dir", "app_environ_dir"},
        {"pm_tiny_log_level", "log_level"},
        {"pm_tiny_log_max_size_kb", "log_max_size_kb"},
        {"pm_tiny_log_archive_count", "log_archive_count"},
    };
    for (const auto &entry : common_sources)
        if (root[entry.first]) config.sources[entry.second] = daemon_config_source::config_file;
    const std::pair<const char *, const char *> platform_sources[] = {
        {"pm_tiny_sock_file", "socket_file"},
        {"pm_tiny_uds_abstract_namespace", "uds_abstract_namespace"},
        {"pm_tiny_process_tree_mode", "process_tree_mode"},
        {"pm_tiny_cgroup_root", "cgroup_root"},
        {"pm_tiny_allowed_uids", "allowed_uids"},
        {"pm_tiny_allowed_gids", "allowed_gids"},
        {"pm_tiny_pipe_name", "pipe_name"}, {"pm_tiny_pipe_sddl", "pipe_sddl"},
    };
    for (const auto &entry : platform_sources)
        if (root[entry.first]) config.sources[entry.second] = daemon_config_source::config_file;
    for (auto *path_value : {&config.home_dir, &config.log_file, &config.program_config_file,
                             &config.app_log_dir, &config.app_environ_dir})
        resolve_relative(path, *path_value, platform);
    if (platform == daemon_platform::posix && !config.uds_abstract_namespace)
        resolve_relative(path, config.socket_file, platform);
    config.config_loaded = true;
    return true;
}

bool apply_environment(daemon_config &config, daemon_platform platform, std::string &error) {
    auto override_string = [&](const char *name, const char *field, std::string &target) {
        const auto value = daemon_environment(name);
        if (!value.empty()) {
            target = value;
            config.sources[field] = daemon_config_source::environment;
        }
    };
    override_string(PM_TINY_HOME, "home_dir", config.home_dir);
    override_string(PM_TINY_LOG_FILE, "log_file", config.log_file);
    override_string(PM_TINY_PROG_CFG_FILE, "program_config_file", config.program_config_file);
    override_string(PM_TINY_APP_LOG_DIR, "app_log_dir", config.app_log_dir);
    override_string(PM_TINY_APP_ENVIRON_DIR, "app_environ_dir", config.app_environ_dir);
    const std::string level = daemon_environment(PM_TINY_LOG_LEVEL);
    if (!level.empty()) {
        if (!valid_log_level(level)) {
            error = std::string(PM_TINY_LOG_LEVEL) + " must be debug, info, warn, error, or fatal";
            return false;
        }
        config.log_level = level;
        config.sources["log_level"] = daemon_config_source::environment;
    }
    const std::string max_size = daemon_environment(PM_TINY_LOG_MAX_SIZE_KB);
    if (!max_size.empty()) {
        long parsed = 0;
        if (!parse_long(max_size, 1, 1048576, parsed)) {
            error = std::string(PM_TINY_LOG_MAX_SIZE_KB) + " must be between 1 and 1048576";
            return false;
        }
        config.log_max_size_kb = static_cast<int>(parsed);
        config.sources["log_max_size_kb"] = daemon_config_source::environment;
    }
    const std::string archives = daemon_environment(PM_TINY_LOG_ARCHIVE_COUNT);
    if (!archives.empty()) {
        long parsed = 0;
        if (!parse_long(archives, 0, 100, parsed)) {
            error = std::string(PM_TINY_LOG_ARCHIVE_COUNT) + " must be between 0 and 100";
            return false;
        }
        config.log_archive_count = static_cast<int>(parsed);
        config.sources["log_archive_count"] = daemon_config_source::environment;
    }
    if (platform == daemon_platform::windows) {
        override_string(PM_TINY_PIPE_NAME, "pipe_name", config.pipe_name);
        override_string(PM_TINY_PIPE_SDDL, "pipe_sddl", config.pipe_sddl);
    } else {
        override_string(PM_TINY_SOCK_FILE, "socket_file", config.socket_file);
        override_string(PM_TINY_PROCESS_TREE_MODE, "process_tree_mode", config.process_tree_mode);
        override_string(PM_TINY_CGROUP_ROOT, "cgroup_root", config.cgroup_root);
        const std::string abstract_value = daemon_environment(PM_TINY_UDS_ABSTRACT_NAMESPACE);
        if (!abstract_value.empty()) {
            if (!parse_bool(abstract_value, config.uds_abstract_namespace)) {
                error = std::string(PM_TINY_UDS_ABSTRACT_NAMESPACE) + " must be a boolean";
                return false;
            }
            config.sources["uds_abstract_namespace"] = daemon_config_source::environment;
        }
    }
    return true;
}

} // namespace

daemon_argument_result parse_daemon_arguments(const std::vector<std::string> &arguments,
                                              daemon_platform platform) {
    daemon_argument_result result;
    result.success = false;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const std::string &arg = arguments[i];
        const auto require_value = [&](std::string &target) -> bool {
            if (i + 1 >= arguments.size()) {
                result.error = arg + " requires a value";
                return false;
            }
            target = arguments[++i];
            return true;
        };
        if (arg == "-c" || arg == "--config") {
            if (!require_value(result.options.config_path)) return result;
            result.options.config_explicit = true;
        } else if (arg == "--home") {
            if (!require_value(result.options.home_dir)) return result;
            if (result.options.home_dir.empty()) { result.error = "--home must not be empty"; return result; }
            result.options.home_explicit = true;
        } else if (arg == "--log-level") {
            if (!require_value(result.options.log_level)) return result;
            if (!valid_log_level(result.options.log_level)) {
                result.error = "--log-level must be debug, info, warn, error, or fatal";
                return result;
            }
            result.options.log_level_explicit = true;
        } else if (arg == "--log-max-size-kb") {
            std::string value;
            if (!require_value(value)) return result;
            if (!parse_long(value, 1, 1048576, result.options.log_max_size_kb)) {
                result.error = "--log-max-size-kb must be between 1 and 1048576";
                return result;
            }
            result.options.log_max_size_explicit = true;
        } else if (arg == "--log-archive-count") {
            std::string value;
            long parsed = 0;
            if (!require_value(value)) return result;
            if (!parse_long(value, 0, 100, parsed)) {
                result.error = "--log-archive-count must be between 0 and 100";
                return result;
            }
            result.options.log_archive_count = static_cast<int>(parsed);
            result.options.log_archive_count_explicit = true;
        } else if (arg == "--help" || arg == "-h") {
            result.options.help = true;
        } else if (arg == "--version") {
            result.options.version = true;
        } else if (platform == daemon_platform::posix && (arg == "-d" || arg == "--daemon")) {
            result.options.daemonize = true;
        } else if (platform == daemon_platform::windows && arg == "--service") {
            result.options.service = true;
        } else if (platform == daemon_platform::windows && arg == "--service-name") {
            if (!require_value(result.options.service_name)) return result;
            if (result.options.service_name.empty()) { result.error = "--service-name must not be empty"; return result; }
            result.options.service_name_explicit = true;
        } else if (platform == daemon_platform::windows && arg == "--pipe-name") {
            if (!require_value(result.options.pipe_name)) return result;
            if (result.options.pipe_name.empty()) { result.error = "--pipe-name must not be empty"; return result; }
        } else if (platform == daemon_platform::windows && arg == "--pipe-sddl") {
            if (!require_value(result.options.pipe_sddl)) return result;
            if (result.options.pipe_sddl.empty()) { result.error = "--pipe-sddl must not be empty"; return result; }
        } else {
            result.error = "Unknown option: " + arg;
            return result;
        }
    }
    result.success = true;
    return result;
}

daemon_argument_result parse_daemon_arguments(int argc, char *argv[], daemon_platform platform) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) arguments.emplace_back(argv[i] == nullptr ? "" : argv[i]);
    return parse_daemon_arguments(arguments, platform);
}

daemon_config_result resolve_daemon_config(const daemon_cli_options &options,
                                           daemon_platform platform) {
    daemon_config_result result;
    auto &config = result.config;
    const char *all_fields[] = {
        "config_path", "home_dir", "lock_file", "log_file", "program_config_file",
        "app_log_dir", "app_environ_dir", "log_level", "log_max_size_kb",
        "log_archive_count", "socket_file", "uds_abstract_namespace",
        "process_tree_mode", "cgroup_root", "allowed_uids", "allowed_gids",
        "pipe_name", "pipe_sddl"
    };
    for (const auto *field : all_fields) config.sources[field] = daemon_config_source::default_value;
#ifdef PM_TINY_UDS_ABSTRACT_NAMESPACE_DEFAULT
    if (platform == daemon_platform::posix) config.uds_abstract_namespace = true;
#endif
    config.config_path = options.config_explicit ? options.config_path :
                         implicit_config_path(options, platform);
    if (options.config_explicit) config.sources["config_path"] = daemon_config_source::command_line;
    if (options.config_explicit || file_exists(config.config_path)) {
        if (!load_file_config(config.config_path, platform, config, result.error)) return result;
    }
    if (!valid_log_level(config.log_level)) {
        result.error = "pm_tiny_log_level must be debug, info, warn, error, or fatal";
        return result;
    }
    if (config.log_max_size_kb < 1 || config.log_max_size_kb > 1048576) {
        result.error = "pm_tiny_log_max_size_kb must be between 1 and 1048576";
        return result;
    }
    if (config.log_archive_count < 0 || config.log_archive_count > 100) {
        result.error = "pm_tiny_log_archive_count must be between 0 and 100";
        return result;
    }
    if (!apply_environment(config, platform, result.error)) return result;
    if (options.home_explicit) {
        config.home_dir = options.home_dir;
        config.sources["home_dir"] = daemon_config_source::command_line;
    }
    if (options.log_level_explicit) {
        config.log_level = options.log_level;
        config.sources["log_level"] = daemon_config_source::command_line;
    }
    if (options.log_max_size_explicit) {
        config.log_max_size_kb = static_cast<int>(options.log_max_size_kb);
        config.sources["log_max_size_kb"] = daemon_config_source::command_line;
    }
    if (options.log_archive_count_explicit) {
        config.log_archive_count = options.log_archive_count;
        config.sources["log_archive_count"] = daemon_config_source::command_line;
    }
    if (platform == daemon_platform::windows) {
        if (!options.pipe_name.empty()) {
            config.pipe_name = options.pipe_name;
            config.sources["pipe_name"] = daemon_config_source::command_line;
        }
        if (!options.pipe_sddl.empty()) {
            config.pipe_sddl = options.pipe_sddl;
            config.sources["pipe_sddl"] = daemon_config_source::command_line;
        }
    }
    if (config.home_dir.empty()) config.home_dir = default_home(platform);
    trim_trailing_separators(config.home_dir, platform);
    trim_trailing_separators(config.app_log_dir, platform);
    trim_trailing_separators(config.app_environ_dir, platform);
    if (config.log_file.empty()) {
        config.log_file = join_path(config.home_dir, "pm_tiny.log", platform);
        config.sources["log_file"] = daemon_config_source::derived;
    }
    if (config.program_config_file.empty()) {
        config.program_config_file = join_path(config.home_dir, "prog.yaml", platform);
        config.sources["program_config_file"] = daemon_config_source::derived;
    }
    if (config.app_log_dir.empty()) {
        config.app_log_dir = join_path(config.home_dir, "logs", platform);
        config.sources["app_log_dir"] = daemon_config_source::derived;
    }
    if (config.app_environ_dir.empty()) {
        config.app_environ_dir = join_path(config.home_dir, "environ", platform);
        config.sources["app_environ_dir"] = daemon_config_source::derived;
    }
    config.lock_file = join_path(config.home_dir, "pm_tiny.pid", platform);
    config.sources["lock_file"] = daemon_config_source::derived;
    if (platform == daemon_platform::posix && config.socket_file.empty()) {
        config.socket_file = config.uds_abstract_namespace ? "pm_tinyd" :
                             join_path(config.home_dir, "pm_tinyd.sock", platform);
        config.sources["socket_file"] = daemon_config_source::derived;
    }
    if (config.program_config_file.empty()) {
        result.error = "pm_tiny_prog_cfg_file must not be empty";
        return result;
    }
    if (platform == daemon_platform::windows && config.pipe_name.empty()) {
        result.error = "pm_tiny_pipe_name must not be empty";
        return result;
    }
    if (platform == daemon_platform::windows && config.pipe_sddl.empty()) {
        result.error = "pm_tiny_pipe_sddl must not be empty";
        return result;
    }
    result.success = true;
    return result;
}

const char *daemon_config_source_name(daemon_config_source source) {
    switch (source) {
        case daemon_config_source::default_value: return "default";
        case daemon_config_source::config_file: return "config_file";
        case daemon_config_source::environment: return "environment";
        case daemon_config_source::command_line: return "command_line";
        case daemon_config_source::derived: return "derived";
    }
    return "default";
}

daemon_config_source daemon_config::source_of(const std::string &field) const {
    const auto iter = sources.find(field);
    return iter == sources.end() ? daemon_config_source::default_value : iter->second;
}

std::string daemon_usage(const std::string &program, daemon_platform platform) {
    std::ostringstream out;
    out << "Usage: " << program << " [options]\n\n"
        << "  -c, --config PATH            daemon configuration file\n"
        << "      --home PATH              PM_TINY_HOME override\n"
        << "      --log-level LEVEL        debug, info, warn, error, or fatal\n"
        << "      --log-max-size-kb N      1..1048576\n"
        << "      --log-archive-count N    0..100\n"
        << "  -h, --help                   show this help\n"
        << "      --version                show version\n";
    if (platform == daemon_platform::posix) {
        out << "  -d, --daemon                 run in background\n";
    } else {
        out << "      --service                run under Windows SCM\n"
            << "      --service-name NAME      SCM service name\n"
            << "      --pipe-name NAME         named pipe override\n"
            << "      --pipe-sddl SDDL         named pipe ACL override\n";
    }
    return out.str();
}

std::string daemon_environment(const std::string &name) {
#ifdef _WIN32
    try {
        const std::wstring wide_name = utf8_to_wide(name);
        const DWORD required = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
        if (required == 0) return {};
        std::wstring value(required, L'\0');
        const DWORD written = GetEnvironmentVariableW(wide_name.c_str(), &value[0], required);
        if (written == 0 || written >= required) return {};
        value.resize(written);
        return wide_to_utf8(value);
    } catch (...) { return {}; }
#else
    const char *value = std::getenv(name.c_str());
    return value == nullptr ? std::string() : std::string(value);
#endif
}

bool set_daemon_environment(const std::string &name, const std::string &value,
                            std::string &error) {
#ifdef _WIN32
    try {
        if (SetEnvironmentVariableW(utf8_to_wide(name).c_str(), utf8_to_wide(value).c_str())) return true;
        error = "SetEnvironmentVariableW failed for " + name + ": " + std::to_string(GetLastError());
        return false;
    } catch (const std::exception &ex) {
        error = "Cannot set " + name + ": " + ex.what();
        return false;
    }
#else
    if (setenv(name.c_str(), value.c_str(), 1) == 0) return true;
    error = "setenv failed for " + name;
    return false;
#endif
}

} // namespace pm_tiny
