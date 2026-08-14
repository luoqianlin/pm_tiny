#include "rotating_log_writer.h"

#include <windows.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace pm_tiny {
namespace {

std::string win_error(const std::string &operation, DWORD code = GetLastError()) {
    return operation + " failed with error code " + std::to_string(code);
}

std::wstring utf8_to_wide(const std::string &value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error(win_error("MultiByteToWideChar"));
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), &result[0], count) != count)
        throw std::runtime_error(win_error("MultiByteToWideChar"));
    return result;
}

std::wstring parent_path(const std::wstring &path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool ensure_directory(const std::wstring &path, std::string &error) {
    if (path.empty()) return true;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) return true;
        error = "log parent path is not a directory";
        return false;
    }
    if (!ensure_directory(parent_path(path), error)) return false;
    if (CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) return true;
    error = win_error("CreateDirectoryW");
    return false;
}

} // namespace

rotating_log_writer::rotating_log_writer(std::string path,
                                         std::size_t max_size_bytes,
                                         int archive_count)
    : path_(std::move(path)),
      max_size_bytes_(max_size_bytes),
      archive_count_(archive_count) {}

rotating_log_writer::~rotating_log_writer() { close(); }

bool rotating_log_writer::open(std::string &error) {
    close();
    if (path_.empty() || max_size_bytes_ == 0 || archive_count_ < 0) {
        error = "invalid rotating log writer configuration";
        return false;
    }
    std::wstring path;
    try { path = utf8_to_wide(path_); }
    catch (const std::exception &ex) { error = ex.what(); return false; }
    if (!ensure_directory(parent_path(path), error)) return false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = win_error("CreateFileW");
        return false;
    }
    LARGE_INTEGER size {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > std::numeric_limits<std::size_t>::max()) {
        error = win_error("GetFileSizeEx");
        CloseHandle(file);
        return false;
    }
    LARGE_INTEGER end {};
    if (!SetFilePointerEx(file, end, nullptr, FILE_END)) {
        error = win_error("SetFilePointerEx");
        CloseHandle(file);
        return false;
    }
    handle_ = file;
    current_size_ = static_cast<std::size_t>(size.QuadPart);
    error.clear();
    return true;
}

bool rotating_log_writer::rotate(std::string &error) {
    close();
    std::wstring path;
    try { path = utf8_to_wide(path_); }
    catch (const std::exception &ex) { error = ex.what(); return false; }
    if (archive_count_ == 0) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                  nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { error = win_error("CreateFileW"); return false; }
        handle_ = file;
        current_size_ = 0;
        return true;
    }
    for (int index = archive_count_; index >= 1; --index) {
        const std::wstring source = index == 1 ? path : path + L"." + std::to_wstring(index - 1);
        const std::wstring target = path + L"." + std::to_wstring(index);
        if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            const DWORD code = GetLastError();
            if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
                error = win_error("MoveFileExW", code);
                return false;
            }
        }
    }
    return open(error);
}

bool rotating_log_writer::append(const char *data, std::size_t size, std::string &error) {
    if (data == nullptr || size == 0) return true;
    if (handle_ == nullptr && !open(error)) return false;
    while (size > 0) {
        if (current_size_ >= max_size_bytes_ && !rotate(error)) return false;
        const std::size_t chunk = std::min(size, max_size_bytes_ - current_size_);
        std::size_t offset = 0;
        while (offset < chunk) {
            const DWORD request = static_cast<DWORD>(std::min<std::size_t>(chunk - offset, MAXDWORD));
            DWORD written = 0;
            if (!WriteFile(static_cast<HANDLE>(handle_), data + offset, request, &written, nullptr) || written == 0) {
                error = win_error("WriteFile");
                close();
                return false;
            }
            offset += written;
        }
        data += chunk;
        size -= chunk;
        current_size_ += chunk;
    }
    error.clear();
    return true;
}

bool rotating_log_writer::flush(std::string &error) {
    if (handle_ == nullptr) return true;
    if (FlushFileBuffers(static_cast<HANDLE>(handle_))) return true;
    error = win_error("FlushFileBuffers");
    return false;
}

void rotating_log_writer::close() {
    if (handle_ != nullptr) CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
    current_size_ = 0;
}

} // namespace pm_tiny
