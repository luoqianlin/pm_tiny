//
// Created by qianlinluo@foxmail.com on 22-8-17.
//

#ifndef PM_TINY_STRTOOLS_H
#define PM_TINY_STRTOOLS_H
namespace pm_tiny {
    inline constexpr const char *file_name(const char *path) {
        const char *file = path;
        while (*path) {
            if (*path++ == '/') {
                file = path;
            }
        }
        return file;
    }
}

//https://blog.galowicz.de/2016/02/20/short_file_macro/
#define PM_TINY_SHORT_FILE ({constexpr const char* sf__ {pm_tiny::file_name(__FILE__)}; sf__;})

#endif //PM_TINY_STRTOOLS_H
