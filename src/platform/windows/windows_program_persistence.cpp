#include "windows_program_persistence.h"

#include "core/daemon_config.h"
#include "core/prog_cfg_yaml_helper.h"
#include "win_utils.h"

#include <yaml-cpp/yaml.h>

#include <windows.h>

#include <algorithm>
#include <ctime>
#include <sstream>

namespace pm_tiny {
namespace win {
namespace {

struct save_transaction {
    std::string state;
    std::string suffix;
    std::string config_path;
    std::string config_temp;
    std::string config_backup;
    std::string environ_path;
    std::string environ_stage;
    std::string environ_backup;
    bool had_config = false;
    bool had_environ = false;
};

bool path_exists(const std::string &path) {
    return GetFileAttributesW(utf8_to_wide(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool is_directory(const std::string &path) {
    const DWORD attributes = GetFileAttributesW(utf8_to_wide(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::string parent_directory(const std::string &path) {
    const auto slash = path.find_last_of("\\/");
    if (slash == std::string::npos) return ".";
    if (slash == 0) return path.substr(0, 1);
    return path.substr(0, slash);
}

bool flush_directory(const std::string &path) {
    HANDLE directory = CreateFileW(utf8_to_wide(path).c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (directory == INVALID_HANDLE_VALUE) return true;
    const bool flushed = FlushFileBuffers(directory) != FALSE;
    const DWORD error = flushed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(directory);
    return flushed || error == ERROR_INVALID_FUNCTION || error == ERROR_INVALID_HANDLE ||
           error == ERROR_ACCESS_DENIED;
}

bool write_file(const std::string &path, const std::string &content, std::string &error) {
    HANDLE file = CreateFileW(utf8_to_wide(path).c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "Cannot create `" + path + "`: " + std::to_string(GetLastError());
        return false;
    }
    std::size_t offset = 0;
    while (offset < content.size()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(content.size() - offset, 1024 * 1024));
        if (!WriteFile(file, content.data() + offset, chunk, &written, nullptr) || written == 0) {
            error = "Cannot write `" + path + "`: " + std::to_string(GetLastError());
            CloseHandle(file);
            return false;
        }
        offset += written;
    }
    if (!FlushFileBuffers(file)) {
        error = "Cannot flush `" + path + "`: " + std::to_string(GetLastError());
        CloseHandle(file);
        return false;
    }
    CloseHandle(file);
    return true;
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

void remove_flat_directory(const std::string &path) {
    if (!is_directory(path)) return;
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = utf8_to_wide(path + "\\*");
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            DeleteFileW(utf8_to_wide(path + "\\" + wide_to_utf8(name)).c_str());
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    RemoveDirectoryW(utf8_to_wide(path).c_str());
}

bool move_path(const std::string &from, const std::string &to, bool replace, std::string &error) {
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (replace) flags |= MOVEFILE_REPLACE_EXISTING;
    if (MoveFileExW(utf8_to_wide(from).c_str(), utf8_to_wide(to).c_str(), flags)) return true;
    error = "Cannot move `" + from + "` to `" + to + "`: " + std::to_string(GetLastError());
    return false;
}

bool write_journal(const std::string &journal, const save_transaction &transaction,
                   std::string &error) {
    YAML::Node root;
    root["schema"] = 1;
    root["state"] = transaction.state;
    root["suffix"] = transaction.suffix;
    root["config_path"] = transaction.config_path;
    root["config_temp"] = transaction.config_temp;
    root["config_backup"] = transaction.config_backup;
    root["environ_path"] = transaction.environ_path;
    root["environ_stage"] = transaction.environ_stage;
    root["environ_backup"] = transaction.environ_backup;
    root["had_config"] = transaction.had_config;
    root["had_environ"] = transaction.had_environ;
    YAML::Emitter emitter;
    emitter << root;
    const std::string temporary = journal + ".tmp";
    if (!write_file(temporary, std::string(emitter.c_str()) + "\n", error)) return false;
    if (!move_path(temporary, journal, true, error)) {
        DeleteFileW(utf8_to_wide(temporary).c_str());
        return false;
    }
    return flush_directory(parent_directory(journal));
}

bool read_journal(const std::string &journal, save_transaction &transaction,
                  std::string &error) {
    std::string content;
    if (!read_file(journal, content, error)) return false;
    try {
        const YAML::Node root = YAML::Load(content);
        if (!root.IsMap() || !root["schema"] || root["schema"].as<int>() != 1) {
            error = "unsupported save journal schema";
            return false;
        }
        transaction.state = root["state"].as<std::string>();
        transaction.suffix = root["suffix"].as<std::string>();
        transaction.config_path = root["config_path"].as<std::string>();
        transaction.config_temp = root["config_temp"].as<std::string>();
        transaction.config_backup = root["config_backup"].as<std::string>();
        transaction.environ_path = root["environ_path"].as<std::string>();
        transaction.environ_stage = root["environ_stage"].as<std::string>();
        transaction.environ_backup = root["environ_backup"].as<std::string>();
        transaction.had_config = root["had_config"].as<bool>();
        transaction.had_environ = root["had_environ"].as<bool>();
    } catch (const YAML::Exception &ex) {
        error = ex.what();
        return false;
    }
    return true;
}

bool valid_transaction(const save_transaction &transaction, const std::string &config_path,
                       const std::string &environ_path) {
    return !transaction.suffix.empty() && transaction.suffix.find(".txn.") == 0 &&
           transaction.config_path == config_path && transaction.environ_path == environ_path &&
           transaction.config_temp == config_path + transaction.suffix &&
           transaction.config_backup == config_path + ".bak" + transaction.suffix &&
           transaction.environ_stage == environ_path + transaction.suffix &&
           transaction.environ_backup == environ_path + ".bak" + transaction.suffix &&
           (transaction.state == "prepared" || transaction.state == "old_moved" ||
            transaction.state == "new_installed");
}

bool restore_backup(const std::string &backup, const std::string &path, bool had_original,
                    bool directory, std::string &error) {
    if (path_exists(backup)) {
        if (directory) remove_flat_directory(path);
        else DeleteFileW(utf8_to_wide(path).c_str());
        return move_path(backup, path, false, error);
    }
    if (!had_original) {
        if (directory) remove_flat_directory(path);
        else DeleteFileW(utf8_to_wide(path).c_str());
    }
    return !had_original || path_exists(path);
}

bool fail_at(const char *step) {
#ifdef PM_TINY_TESTING
    return daemon_environment("PM_TINY_TEST_FAIL_SAVE_STEP") == step;
#else
    (void)step;
    return false;
#endif
}

} // namespace

bool recover_program_config_save(const std::string &program_config_path,
                                 const std::string &app_environ_dir,
                                 std::string &error) {
    const std::string journal = program_config_path + ".save-journal";
    if (!path_exists(journal)) return true;
    save_transaction transaction;
    if (!read_journal(journal, transaction, error) ||
        !valid_transaction(transaction, program_config_path, app_environ_dir)) {
        if (error.empty()) error = "save journal paths or state are invalid";
        error = "Cannot recover `" + journal + "`: " + error;
        return false;
    }
    if (transaction.state == "new_installed") {
        if (!path_exists(transaction.config_path) || !path_exists(transaction.environ_path)) {
            error = "Cannot recover committed save transaction: installed files are missing";
            return false;
        }
        DeleteFileW(utf8_to_wide(transaction.config_backup).c_str());
        remove_flat_directory(transaction.environ_backup);
    } else {
        if (!restore_backup(transaction.config_backup, transaction.config_path,
                            transaction.had_config, false, error) ||
            !restore_backup(transaction.environ_backup, transaction.environ_path,
                            transaction.had_environ, true, error)) return false;
    }
    DeleteFileW(utf8_to_wide(transaction.config_temp).c_str());
    remove_flat_directory(transaction.environ_stage);
    DeleteFileW(utf8_to_wide(journal).c_str());
    return flush_directory(parent_directory(journal));
}

int save_program_configs(const std::vector<ProgramConfig> &configs,
                         const std::string &program_config_path,
                         const std::string &app_environ_dir,
                         std::string &error) {
    if (!recover_program_config_save(program_config_path, app_environ_dir, error)) return -1;
    const std::string suffix = ".txn." + std::to_string(GetCurrentProcessId()) + "." +
                               std::to_string(static_cast<long long>(std::time(nullptr)));
    save_transaction transaction;
    transaction.state = "prepared";
    transaction.suffix = suffix;
    transaction.config_path = program_config_path;
    transaction.config_temp = program_config_path + suffix;
    transaction.config_backup = program_config_path + ".bak" + suffix;
    transaction.environ_path = app_environ_dir;
    transaction.environ_stage = app_environ_dir + suffix;
    transaction.environ_backup = app_environ_dir + ".bak" + suffix;
    transaction.had_config = path_exists(program_config_path);
    transaction.had_environ = path_exists(app_environ_dir);
    remove_flat_directory(transaction.environ_stage);
    remove_flat_directory(transaction.environ_backup);
    DeleteFileW(utf8_to_wide(transaction.config_backup).c_str());
    if (!CreateDirectoryW(utf8_to_wide(transaction.environ_stage).c_str(), nullptr)) {
        error = "Cannot create environment staging directory: " + std::to_string(GetLastError());
        return -1;
    }
    YAML::Node config_root;
    for (const auto &config : configs) {
        ProgCfgSerializeOptions options;
        options.include_run_as = false;
        options.include_oom_score_adj = false;
        options.include_pty = false;
        config_root.push_back(serialize_prog_cfg_yaml_node(config, options));
        YAML::Node environment_root;
        environment_root["schema"] = 1;
        environment_root["environment"] = config.envs;
        YAML::Emitter environment;
        environment << environment_root;
        if (!write_file(transaction.environ_stage + "\\" + config.name + ".yaml",
                        std::string(environment.c_str()) + "\n", error)) {
            remove_flat_directory(transaction.environ_stage);
            return -1;
        }
    }
    YAML::Emitter config;
    if (configs.empty()) config << YAML::BeginSeq << YAML::EndSeq;
    else config << config_root;
    if (!write_file(transaction.config_temp, std::string(config.c_str()) + "\n", error)) {
        remove_flat_directory(transaction.environ_stage);
        return -1;
    }
    const std::string journal = program_config_path + ".save-journal";
    if (!write_journal(journal, transaction, error)) {
        DeleteFileW(utf8_to_wide(transaction.config_temp).c_str());
        remove_flat_directory(transaction.environ_stage);
        return -1;
    }
    if (fail_at("prepared")) return -1;
    const auto rollback = [&]() {
        std::string recovery_error;
        if (!recover_program_config_save(program_config_path, app_environ_dir, recovery_error)) {
            if (!error.empty()) error += "; ";
            error += recovery_error;
        }
        return -1;
    };
    if (transaction.had_config &&
        !move_path(transaction.config_path, transaction.config_backup, false, error)) return rollback();
    if (transaction.had_environ &&
        !move_path(transaction.environ_path, transaction.environ_backup, false, error)) return rollback();
    transaction.state = "old_moved";
    if (!write_journal(journal, transaction, error)) return rollback();
    if (fail_at("old_moved")) return -1;
    if (!move_path(transaction.config_temp, transaction.config_path, false, error) ||
        !move_path(transaction.environ_stage, transaction.environ_path, false, error)) return rollback();
    transaction.state = "new_installed";
    if (!write_journal(journal, transaction, error)) return rollback();
    if (fail_at("new_installed")) return -1;
    DeleteFileW(utf8_to_wide(transaction.config_backup).c_str());
    remove_flat_directory(transaction.environ_backup);
    DeleteFileW(utf8_to_wide(journal).c_str());
    flush_directory(parent_directory(program_config_path));
    return 0;
}

} // namespace win
} // namespace pm_tiny
