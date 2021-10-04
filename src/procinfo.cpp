//
// Created by qianlinluo@foxmail.com on 2022/6/27.
//
#include "procinfo.h"
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <fstream>
#include <string>
#include "string_utils.h"
#include "globals.h"

namespace pm_tiny {
    namespace utils {
        namespace proc {
            static int read_cmdline(const char *file,
                                    std::vector<std::string> &cmdlines) {
                std::string str;
                FILE *fp = fopen(file, "r");
                if (fp) {
                    while (true) {
                        int ch = fgetc(fp);
                        if (ch == EOF)break;
                        if (ch == 0) {
                            if (!str.empty()) {
                                cmdlines.emplace_back(str);
                                str = "";
                            }
                        } else {
                            str += ((char) ch);
                        }
                    }
                    fclose(fp);
                    return 0;
                }
                return -1;
            }

            static int read_file_content(const char *file,
                                         std::string &content) {
                std::string str;
                FILE *fp = fopen(file, "r");
                if (fp) {
                    while (true) {
                        int ch = fgetc(fp);
                        if (ch == EOF)break;
                        str += ((char) ch);
                    }
                    fclose(fp);
                    content=str;
                    return 0;
                }
                return -1;
            }

            int get_proc_info(int pid, procinfo_t &procinfo) {
                char path[200];
                char _exepath[PATH_MAX];
                memset(_exepath, 0, sizeof(_exepath));
                snprintf(path, sizeof(path), "%s/%d/exe",procdir_path, pid);
                auto rc = readlink(path, _exepath, sizeof(_exepath));
                if (rc != -1) {
                    procinfo.exe_path = std::string(_exepath);
                }
                snprintf(path, sizeof(path), "%s/%d/cmdline",procdir_path, pid);
                read_cmdline(path, procinfo.cmdline);
                snprintf(path, sizeof(path), "%s/%d/comm", procdir_path,pid);
                read_file_content(path, procinfo.comm);
                mgr::utils::trim(procinfo.comm);
                return rc != -1 ? 0 : -1;
            }
        }
    }
}

