#include "posix_privilege_wrapper.h"

#include <cstdlib>
#include <iostream>

namespace {

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << std::endl;
        std::abort();
    }
}

} // namespace

int main() {
    expect(pm_tiny::cli::is_privilege_wrapper_executable("sudo"), "sudo should be recognized");
    expect(pm_tiny::cli::is_privilege_wrapper_executable("/usr/bin/su"), "su path should be recognized");
    expect(pm_tiny::cli::is_privilege_wrapper_executable("/usr/local/bin/doas"),
           "doas path should be recognized");
    expect(!pm_tiny::cli::is_privilege_wrapper_executable("sudo-helper"),
           "ordinary executable should not be recognized as privilege wrapper");
    return 0;
}
