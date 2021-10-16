//
// Created by qianlinluo@foxmail.com on 2022/6/27.
//

#ifndef PM_TINY_PROCINFO_H
#define PM_TINY_PROCINFO_H
#include <string>
#include <vector>

namespace pm_tiny {
    namespace utils {
        namespace proc {
            struct procinfo_t {
                std::string exe_path;
                std::vector<std::string> cmdline;
                std::string comm;
            };

            int get_proc_info(int pid, procinfo_t &procinfo);
        }
    }
}
#endif //PM_TINY_PROCINFO_H
