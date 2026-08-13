#include "log_writer.h"

#include "win_utils.h"

#include <windows.h>

#include <stdexcept>
#include <string>

namespace pm_tiny {
namespace win {

namespace {
std::wstring join_paths(const std::wstring &lhs, const std::wstring &rhs) {
    if (lhs.empty()) {
        return rhs;
    }
    if (lhs.back() == L'\\' || lhs.back() == L'/') {
        return lhs + rhs;
    }
    return lhs + L"\\" + rhs;
}

bool ensure_directory_exists(const std::wstring &path) {
    if (path.empty()) {
        return true;
    }
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return true;
    }
    if (CreateDirectoryW(path.c_str(), nullptr)) {
        return true;
    }
    DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS) {
        return true;
    }
    return false;
}

} // namespace

LogWriter::LogWriter(std::string directory,
                     std::string file_name,
                     std::size_t max_size_bytes,
                     int max_files)
    : directory_utf8_(std::move(directory)),
      file_name_utf8_(std::move(file_name)),
      max_size_bytes_(max_size_bytes > 0 ? max_size_bytes : 4 * 1024 * 1024),
      max_files_(max_files > 0 ? max_files : 3) {
    directory_ = utf8_to_wide(directory_utf8_);
    ensure_directory();
    base_path_ = join_paths(directory_, utf8_to_wide(file_name_utf8_));
    open_file();
}

LogWriter::~LogWriter() {
    close_file();
}

void LogWriter::ensure_directory() {
    if (directory_.empty()) {
        return;
    }
    if (!ensure_directory_exists(directory_)) {
        throw std::runtime_error("Failed to create log directory");
    }
}

std::wstring LogWriter::build_log_path(int index) const {
    if (index <= 0) {
        return base_path_;
    }
    std::wstring suffix = L"." + std::to_wstring(index);
    return base_path_ + suffix;
}

void LogWriter::open_file() {
    close_file();
    file_handle_ = CreateFileW(base_path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_handle_ == INVALID_HANDLE_VALUE) {
        file_handle_ = nullptr;
        throw std::runtime_error("Failed to open log file");
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(static_cast<HANDLE>(file_handle_), &size)) {
        current_size_ = static_cast<std::size_t>(size.QuadPart);
    } else {
        current_size_ = 0;
    }
    SetFilePointer(static_cast<HANDLE>(file_handle_), 0, nullptr, FILE_END);
}

void LogWriter::close_file() {
    if (file_handle_ != nullptr && file_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(file_handle_));
        file_handle_ = nullptr;
    }
}

void LogWriter::rotate_files() {
    close_file();
    for (int idx = max_files_ - 1; idx >= 0; --idx) {
        std::wstring src = build_log_path(idx);
        DWORD attrs = GetFileAttributesW(src.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        std::wstring dst = build_log_path(idx + 1);
        MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    open_file();
    current_size_ = 0;
}

void LogWriter::append(const char *data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return;
    }
    if (current_size_ + size > max_size_bytes_) {
        rotate_files();
    }
    DWORD written = 0;
    if (!WriteFile(static_cast<HANDLE>(file_handle_), data, static_cast<DWORD>(size), &written, nullptr)) {
        throw std::runtime_error("Failed to write log file");
    }
    current_size_ += written;
}

void LogWriter::flush() {
    if (file_handle_ != nullptr && file_handle_ != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(static_cast<HANDLE>(file_handle_));
    }
}

} // namespace win
} // namespace pm_tiny
