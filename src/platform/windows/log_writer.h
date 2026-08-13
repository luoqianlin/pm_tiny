#pragma once

#include <string>

namespace pm_tiny {
namespace win {

class LogWriter {
public:
    LogWriter(std::string directory,
              std::string file_name,
              std::size_t max_size_bytes,
              int max_files);
    ~LogWriter();

    LogWriter(const LogWriter &) = delete;
    LogWriter &operator=(const LogWriter &) = delete;

    void append(const char *data, std::size_t size);
    void flush();

private:
    std::string directory_utf8_;
    std::string file_name_utf8_;
    std::size_t max_size_bytes_;
    int max_files_;

    std::wstring directory_;
    std::wstring base_path_;
    void *file_handle_ = nullptr;
    std::size_t current_size_ = 0;

    void open_file();
    void close_file();
    void rotate_files();
    std::wstring build_log_path(int index) const;
    void ensure_directory();
};

} // namespace win
} // namespace pm_tiny
