//
// Created by qianlinluo@foxmail.com on 23-6-29.
//

#ifndef PM_TINY_PM_TINY_ENUM_H
#define PM_TINY_PM_TINY_ENUM_H
namespace pm_tiny {
    enum class failure_action_t {
        SKIP, RESTART,REBOOT
    };
    using failure_action_underlying_t = std::underlying_type_t<pm_tiny::failure_action_t>;
    inline std::string failure_action_to_str(failure_action_t action) {
        switch (action) {
            case failure_action_t::SKIP:
                return "skip";
            case failure_action_t::RESTART:
                return "restart";
            case failure_action_t::REBOOT:
                return "reboot";
        }
        return "";
    }

    inline failure_action_t str_to_failure_action(const std::string &action_str) {
        if (action_str == "skip") {
            return failure_action_t::SKIP;
        } else if (action_str == "restart") {
            return failure_action_t::RESTART;
        } else if (action_str == "reboot") {
            return failure_action_t::REBOOT;
        } else {
            using namespace std::string_literals;
            throw std::invalid_argument("illegal `"s + action_str + "`");
        }
    }

    inline std::ostream &operator<<(std::ostream &os, failure_action_t action) {
        return os << failure_action_to_str(action);
    }
}
#endif //PM_TINY_PM_TINY_ENUM_H
