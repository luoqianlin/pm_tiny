#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {

bool write_all(int fd, const std::string &data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

int pty_probe() {
    char report[128]{};
    const int size = std::snprintf(report, sizeof(report),
                                   "stdin_tty=%d stdout_tty=%d stderr_tty=%d\n",
                                   ::isatty(STDIN_FILENO) ? 1 : 0,
                                   ::isatty(STDOUT_FILENO) ? 1 : 0,
                                   ::isatty(STDERR_FILENO) ? 1 : 0);
    if (size <= 0 || !write_all(STDOUT_FILENO, std::string(report, static_cast<std::size_t>(size)))) return 2;
    if (!write_all(STDOUT_FILENO, "pty-stdout-marker\n")) return 3;
    if (!write_all(STDERR_FILENO, "pty-stderr-marker\n")) return 4;
    return 0;
}

int rotation_probe() {
    for (int index = 0; index < 4; ++index) {
        const std::string stdout_block(1024, static_cast<char>('A' + index));
        const std::string stderr_block(1024, static_cast<char>('a' + index));
        if (!write_all(STDOUT_FILENO, stdout_block)) return 5;
        if (!write_all(STDERR_FILENO, stderr_block)) return 6;
        ::usleep(20000);
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    const std::string mode = argv[1];
    if (mode == "pty") return pty_probe();
    if (mode == "rotation") return rotation_probe();
    return 1;
}
