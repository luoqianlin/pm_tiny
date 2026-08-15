//
// Created by qianlinluo@foxmail.com on 22-8-5.
//

#ifndef PM_TINY_SCOPEGUARD_H
#define PM_TINY_SCOPEGUARD_H
#include <functional>
#include <utility>

// Need two levels of indirection here for __LINE__ to correctly expand
#define PM_TINY_CONCAT2(a, b) a##b
#define PM_TINY_CONCAT(a, b) PM_TINY_CONCAT2(a, b)
#define PM_TINY_ANON_VAR(str) PM_TINY_CONCAT(str, __LINE__)

namespace pm_tiny {

    enum class ScopeGuardExit {};

    class ScopeGuard {
    public:
        explicit ScopeGuard(std::function<void()> fn) {
            fn_ = std::move(fn);
        };

        ~ScopeGuard() {
            if (fn_) {
                fn_();
            }
        };

    private:
        std::function<void()> fn_;
    };

    inline ScopeGuard operator+(ScopeGuardExit, std::function<void()> fn) {
        return ScopeGuard(std::move(fn));
    }

} // namespace pm_tiny

#define PM_TINY_SCOPE_EXIT \
  auto PM_TINY_ANON_VAR(SCOPE_EXIT_STATE) = pm_tiny::ScopeGuardExit() + [&]()
#endif //PM_TINY_SCOPEGUARD_H
