#include <csignal>
#include <cstdlib>
#include <fstream>
#include <unistd.h>
#include <string>

int main(int argc, char **argv) {
    const bool orphan = argc > 1 && std::string(argv[1]) == "orphan";
    const char *path = argc > 2 ? argv[2] : std::getenv("PM_TINY_TREE_PID_FILE");
    pid_t child = ::fork();
    if (child < 0) return 2;
    if (child == 0) {
        std::signal(SIGTERM, SIG_IGN);
        if (path) {
            std::ofstream out(path);
            out << getpid() << "\n";
        }
        for (;;) ::pause();
    }
    if (path) {
        std::ofstream out(path, std::ios::app);
        out << child << "\n";
    }
    if (orphan) return 0;
    std::signal(SIGTERM, SIG_IGN);
    for (;;) ::pause();
}
