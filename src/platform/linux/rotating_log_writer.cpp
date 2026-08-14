#include "rotating_log_writer.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>

namespace pm_tiny {
namespace {

int writer_fd(void *handle) {
    return static_cast<int>(reinterpret_cast<std::intptr_t>(handle)) - 1;
}

void *writer_handle(int fd) {
    return reinterpret_cast<void *>(static_cast<std::intptr_t>(fd + 1));
}

std::string system_error(const std::string &operation) {
    return operation + ": " + std::strerror(errno);
}

bool ensure_directory(const std::string &path, std::string &error) {
    if (path.empty() || path == "." || path == "/") return true;
    struct stat info {};
    if (::stat(path.c_str(), &info) == 0) {
        if (S_ISDIR(info.st_mode)) return true;
        error = path + ": not a directory";
        return false;
    }
    if (errno != ENOENT) {
        error = system_error("stat " + path);
        return false;
    }
    const auto slash = path.find_last_of('/');
    if (slash != std::string::npos && !ensure_directory(path.substr(0, slash), error)) return false;
    if (::mkdir(path.c_str(), 0700) == 0 || errno == EEXIST) return true;
    error = system_error("mkdir " + path);
    return false;
}

bool ensure_parent_directory(const std::string &path, std::string &error) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos || ensure_directory(path.substr(0, slash), error);
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
    if (!ensure_parent_directory(path_, error)) return false;
    const int fd = ::open(path_.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = system_error("open " + path_);
        return false;
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0 || info.st_size < 0) {
        error = system_error("fstat " + path_);
        ::close(fd);
        return false;
    }
    handle_ = writer_handle(fd);
    current_size_ = static_cast<std::size_t>(info.st_size);
    error.clear();
    return true;
}

bool rotating_log_writer::rotate(std::string &error) {
    close();
    if (archive_count_ == 0) {
        const int fd = ::open(path_.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) {
            error = system_error("truncate " + path_);
            return false;
        }
        handle_ = writer_handle(fd);
        current_size_ = 0;
        return true;
    }
    for (int index = archive_count_; index >= 1; --index) {
        const std::string source = index == 1 ? path_ : path_ + "." + std::to_string(index - 1);
        const std::string target = path_ + "." + std::to_string(index);
        if (::rename(source.c_str(), target.c_str()) != 0 && errno != ENOENT) {
            error = system_error("rename " + source + " to " + target);
            return false;
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
            const ssize_t written = ::write(writer_fd(handle_), data + offset, chunk - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                error = system_error("write " + path_);
                close();
                return false;
            }
            offset += static_cast<std::size_t>(written);
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
    if (::fsync(writer_fd(handle_)) == 0) return true;
    error = system_error("fsync " + path_);
    return false;
}

void rotating_log_writer::close() {
    if (handle_ != nullptr) ::close(writer_fd(handle_));
    handle_ = nullptr;
    current_size_ = 0;
}

} // namespace pm_tiny
