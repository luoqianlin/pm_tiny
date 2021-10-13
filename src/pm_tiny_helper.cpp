//
// Created by luo on 2021/10/8.
//
#include <unistd.h>
#include <pwd.h>
#include "pm_tiny_helper.h"

namespace pm_tiny {
    std::string get_pm_tiny_home_dir(const std::string &home_path) {
        std::string pm_tiny_home_dir;
        char *env_pm_tiny_home = getenv("PM_TINY_HOME");
        if (!home_path.empty()) {
            pm_tiny_home_dir = home_path;
        } else if (env_pm_tiny_home) {
            pm_tiny_home_dir = env_pm_tiny_home;
        } else {
            struct passwd *pw = getpwuid(getuid());
            const char *user_homedir = pw->pw_dir;
            pm_tiny_home_dir = std::string(user_homedir) + "/.pm_tiny";
        }
        if (pm_tiny_home_dir[pm_tiny_home_dir.length() - 1] == '/') {
            pm_tiny_home_dir = pm_tiny_home_dir.substr(0, pm_tiny_home_dir.length() - 1);
        }
        return pm_tiny_home_dir;
    }
}
