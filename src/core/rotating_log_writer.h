#ifndef PM_TINY_ROTATING_LOG_WRITER_H
#define PM_TINY_ROTATING_LOG_WRITER_H

#include <cstddef>
#include <string>

namespace pm_tiny {

class rotating_log_writer {
public:
    rotating_log_writer(std::string path, std::size_t max_size_bytes, int archive_count);
    ~rotating_log_writer();

    rotating_log_writer(const rotating_log_writer &) = delete;
    rotating_log_writer &operator=(const rotating_log_writer &) = delete;

    bool open(std::string &error);
    bool append(const char *data, std::size_t size, std::string &error);
    bool flush(std::string &error);
    void close();
    const std::string &path() const { return path_; }

private:
    std::string path_;
    std::size_t max_size_bytes_;
    int archive_count_;
    void *handle_ = nullptr;
    std::size_t current_size_ = 0;

    bool rotate(std::string &error);
};

} // namespace pm_tiny

#endif
