#include "win_utils.h"
#include "core/daemon_config.h"
#include "core/pm_tiny.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <locale>
#include <cwctype>
#include <stdexcept>
#include <vector>
#include <unordered_map>
#include <utility>

namespace pm_tiny {
namespace win {

namespace {

bool is_quote(char ch) {
    return ch == '\'' || ch == '"';
}

} // namespace

std::string trim_copy(const std::string &text) {
    std::string tmp = text;
    trim_inplace(tmp);
    return tmp;
}

void trim_inplace(std::string &text) {
    auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    if (first >= last) {
        text.clear();
    } else {
        text.assign(first, last);
    }
}

std::string strip_quotes(const std::string &text) {
    if (text.size() >= 2 && is_quote(text.front()) && is_quote(text.back())) {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

bool iequals(const std::string &lhs, const std::string &rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

bool parse_bool(const std::string &text, bool default_value) {
    auto trimmed = trim_copy(text);
    if (trimmed.empty()) {
        return default_value;
    }
    if (iequals(trimmed, "true") || iequals(trimmed, "yes") || iequals(trimmed, "1") || iequals(trimmed, "on")) {
        return true;
    }
    if (iequals(trimmed, "false") || iequals(trimmed, "no") || iequals(trimmed, "0") || iequals(trimmed, "off")) {
        return false;
    }
    return default_value;
}

int parse_int(const std::string &text, int default_value) {
    auto trimmed = trim_copy(text);
    if (trimmed.empty()) {
        return default_value;
    }
    try {
        size_t idx = 0;
        int value = std::stoi(trimmed, &idx, 10);
        if (idx != trimmed.size()) {
            return default_value;
        }
        return value;
    } catch (...) {
        return default_value;
    }
}

std::vector<std::string> split_and_trim(const std::string &text, char delimiter) {
    std::vector<std::string> result;
    std::string token;
    for (char ch : text) {
        if (ch == delimiter) {
            trim_inplace(token);
            if (!token.empty()) {
                result.push_back(strip_quotes(token));
            }
            token.clear();
        } else {
            token.push_back(ch);
        }
    }
    trim_inplace(token);
    if (!token.empty()) {
        result.push_back(strip_quotes(token));
    }
    return result;
}

std::vector<std::string> parse_list_value(const std::string &text) {
    auto trimmed = trim_copy(text);
    if (trimmed.empty()) {
        return {};
    }
    if (trimmed.front() == '[' && trimmed.back() == ']') {
        auto content = trimmed.substr(1, trimmed.size() - 2);
        return split_and_trim(content, ',');
    }
    if (trimmed.find(',') != std::string::npos) {
        return split_and_trim(trimmed, ',');
    }
    std::vector<std::string> single;
    single.push_back(strip_quotes(trimmed));
    return single;
}

std::wstring to_upper_copy(const std::wstring &text) {
    std::wstring result = text;
    for (auto &ch : result) {
        ch = static_cast<wchar_t>(std::towupper(ch));
    }
    return result;
}

std::wstring extract_env_key(const std::wstring &entry) {
    auto pos = entry.find(L'=');
    if (pos == std::wstring::npos) {
        return std::wstring();
    }
    return entry.substr(0, pos);
}

std::vector<wchar_t> build_environment_block(const std::vector<std::string> &overrides_utf8) {
    std::vector<std::wstring> entries;
    std::unordered_map<std::wstring, size_t> key_to_index;

    for (const auto &override_utf8 : overrides_utf8) {
        if (override_utf8.empty()) continue;
        std::wstring wide_entry = utf8_to_wide(override_utf8);
        auto key = extract_env_key(wide_entry);
        if (key.empty()) throw std::runtime_error("invalid environment entry: " + override_utf8);
        auto key_upper = to_upper_copy(key);
        auto iter = key_to_index.find(key_upper);
        if (iter != key_to_index.end()) {
            entries[iter->second] = std::move(wide_entry);
        } else {
            key_to_index[key_upper] = entries.size();
            entries.emplace_back(std::move(wide_entry));
        }
    }

    std::vector<wchar_t> block;
    size_t total = 1;
    for (const auto &entry : entries) {
        total += entry.size() + 1;
    }
    block.resize(total, L'\0');
    size_t offset = 0;
    for (const auto &entry : entries) {
        std::copy(entry.begin(), entry.end(), block.begin() + static_cast<std::ptrdiff_t>(offset));
        offset += entry.size();
        block[offset++] = L'\0';
    }
    block[offset] = L'\0';
    return block;
}

std::string control_pipe_name() {
    const std::string configured = daemon_environment(PM_TINY_PIPE_NAME);
    if (!configured.empty()) return configured;
    return "\\\\.\\pipe\\pm_tiny";
}

std::wstring control_pipe_name_wide() {
    return utf8_to_wide(control_pipe_name());
}

std::wstring utf8_to_wide(const std::string &text) {
    if (text.empty()) {
        return std::wstring();
    }
    int count = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) {
        throw std::runtime_error("MultiByteToWideChar failed");
    }
    std::vector<wchar_t> buffer(static_cast<size_t>(count) + 1, L'\0');
    int written = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), buffer.data(), count);
    if (written <= 0) {
        throw std::runtime_error("MultiByteToWideChar failed");
    }
    buffer[static_cast<size_t>(written)] = L'\0';
    return std::wstring(buffer.data());
}

std::string wide_to_utf8(const std::wstring &text) {
    if (text.empty()) {
        return std::string();
    }
    int count = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed");
    }
    std::vector<char> buffer(static_cast<size_t>(count) + 1, '\0');
    int written = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), buffer.data(), count, nullptr, nullptr);
    if (written <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed");
    }
    buffer[static_cast<size_t>(written)] = '\0';
    return std::string(buffer.data());
}

} // namespace win
} // namespace pm_tiny
