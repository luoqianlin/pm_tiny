//
// Created by qianlinluo@foxmail.com on 23-7-11.
//

#ifndef PM_TINY_APPCLIENT_H
#define PM_TINY_APPCLIENT_H

#include <memory>
#include <string>

#ifdef PM_TINY_API_EXPORTS
#include "pm_tiny.h"
#endif

#ifndef PM_TINY_EXPORTS
# define PM_TINY_EXPORTS
#endif

namespace pm_tiny {
    class PM_TINY_EXPORTS AppClient {
    public:
        AppClient();//If it fails, throw a runtime_error exception

        ~AppClient();

        bool is_enable() const;

        std::string get_app_name() const;

        void tick() const;

        void ready() const;

    private:
        class AppClientImpl;

        std::unique_ptr<AppClientImpl> impl_;
    };
}

#endif //PM_TINY_APPCLIENT_H
