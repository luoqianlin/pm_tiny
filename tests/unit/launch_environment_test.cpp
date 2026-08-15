#include "launch_environment.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << std::endl;
        std::abort();
    }
}

bool contains(const std::vector<std::string> &environment, const std::string &entry) {
    return std::find(environment.begin(), environment.end(), entry) != environment.end();
}

bool contains_key(const std::vector<std::string> &environment, const std::string &key) {
    return std::any_of(environment.begin(), environment.end(), [&](const std::string &entry) {
        return entry.compare(0, key.size() + 1, key + "=") == 0;
    });
}

} // namespace

int main() {
    pm_tiny::passwd_t target;
    target.pw_name = "service";
    target.pw_dir = "/srv/service";
    target.pw_shell = "/bin/sh";

    const std::vector<std::string> inherited = {
        "PATH=/untrusted", "HOME=/home/operator", "USER=operator", "LOGNAME=operator",
        "SHELL=/bin/bash", "SUDO_USER=operator", "LD_LIBRARY_PATH=/tmp/lib", "LANG=C",
        "DUPLICATE=old", "PM_TINY_HOME=/forbidden"
    };
    const std::vector<std::string> explicit_values = {
        "PATH=/explicit/bin", "HOME=/explicit/home", "LD_PRELOAD=/explicit/library.so",
        "DUPLICATE=new"
    };
    const auto sanitized = pm_tiny::compose_launch_environment(
        inherited, explicit_values, &target, true);
    expect(contains(sanitized, "HOME=/explicit/home"), "explicit HOME should win");
    expect(contains(sanitized, "USER=service"), "target USER missing");
    expect(contains(sanitized, "LOGNAME=service"), "target LOGNAME missing");
    expect(contains(sanitized, "SHELL=/bin/sh"), "target SHELL missing");
    expect(contains(sanitized, "PATH=/explicit/bin"), "explicit PATH should be restored");
    expect(contains(sanitized, "LD_PRELOAD=/explicit/library.so"), "explicit LD value should be restored");
    expect(contains(sanitized, "LANG=C"), "ordinary inherited variable missing");
    expect(contains(sanitized, "DUPLICATE=new"), "explicit duplicate should win");
    expect(!contains_key(sanitized, "SUDO_USER"), "inherited SUDO variable should be removed");
    expect(!contains_key(sanitized, "LD_LIBRARY_PATH"), "inherited LD variable should be removed");
    expect(!contains_key(sanitized, "PM_TINY_HOME"), "reserved variable should be removed");

    const auto unchanged = pm_tiny::compose_launch_environment(inherited, {}, &target, false);
    expect(contains(unchanged, "PATH=/untrusted"), "same-user PATH should be inherited");
    expect(contains(unchanged, "HOME=/home/operator"), "same-user HOME should be inherited");
    expect(contains(unchanged, "SUDO_USER=operator"), "same-user SUDO variable should be inherited");

    expect(pm_tiny::executable_has_path("/usr/bin/tool"), "absolute path should be accepted");
    expect(pm_tiny::executable_has_path("./tool"), "relative path should be accepted");
    expect(!pm_tiny::executable_has_path("tool"), "bare executable should be rejected");
    return 0;
}
